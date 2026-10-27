#include "engine/editor/imgui_renderer.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

using rhi::BufferDesc;
using rhi::BufferUsage;
using rhi::CommandList;
using rhi::Format;
using rhi::QueueType;
using rhi::RhiStatus;
using rhi::RhiResult;
using rhi::ResourceState;

// The first buffers hold this many vertices; a growth doubles the size, so a busy frame reallocates a few times at most.
constexpr std::uint32_t kMinimumVertexCapacity = 4096;
constexpr std::uint64_t kVertexBytes = sizeof(ImGuiVertex);
constexpr std::uint64_t kIndexBytes = sizeof(std::uint32_t);

}  // namespace

ImGuiRenderer::ImGuiRenderer(rhi::Device& device, bool y_up) : device_(device), y_up_(y_up) {}

RhiResult<std::unique_ptr<ImGuiRenderer>> ImGuiRenderer::create(rhi::Device& device, Format target_format, const ImGuiShaders& shaders) {
    if (shaders.vertex.stage != rhi::ShaderStage::Vertex || shaders.pixel.stage != rhi::ShaderStage::Pixel) {
        return rhi::fail<std::unique_ptr<ImGuiRenderer>>(rhi::RhiErrorCode::InvalidDescriptor,
                                                          "imgui: the vertex and pixel shaders must have their stages");
    }
    // NDC y points up on DX12 and down on Vulkan (engine/rhi/types.hpp), so the expansion writes the sign for the device's backend.
    std::unique_ptr<ImGuiRenderer> renderer(new ImGuiRenderer(device, device.backend() != rhi::Backend::Vulkan));

    // The renderer owns its handles from here on: an early return destroys whatever was created, through the destructor.
    rhi::DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Pixel});
    auto layout = device.create_descriptor_set_layout(layout_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }
    renderer->layout_ = std::move(layout).value();

    auto set = device.create_descriptor_set(renderer->layout_);
    if (!set.ok()) {
        return core::failure(set.error());
    }
    renderer->set_ = std::move(set).value();

    rhi::PipelineDesc pipeline;
    pipeline.kind = rhi::PipelineKind::Graphics;
    pipeline.shaders = {shaders.vertex, shaders.pixel};
    pipeline.set_layouts = {renderer->layout_};
    pipeline.color_formats = {target_format};
    pipeline.depth_format = Format::Unknown;
    pipeline.cull_mode = rhi::CullMode::None;
    pipeline.alpha_blend = true;
    pipeline.depth_write = false;
    pipeline.vertex_stride = static_cast<std::uint32_t>(kVertexBytes);
    pipeline.vertex_attributes = {
        rhi::VertexAttribute{0, Format::R32G32Float, 0},
        rhi::VertexAttribute{1, Format::R32G32Float, 8},
        rhi::VertexAttribute{2, Format::R32Uint, 16},
        rhi::VertexAttribute{3, Format::R32G32B32A32Float, 20},
    };
    pipeline.debug_name = "imgui";
    auto created_pipeline = device.create_pipeline(pipeline);
    if (!created_pipeline.ok()) {
        return core::failure(created_pipeline.error());
    }
    renderer->pipeline_ = std::move(created_pipeline).value();

    auto fence = device.create_fence(0);
    if (!fence.ok()) {
        return core::failure(fence.error());
    }
    renderer->fence_ = std::move(fence).value();
    return core::success(std::move(renderer));
}

ImGuiRenderer::~ImGuiRenderer() {
    destroy_buffers();
    if (font_.valid()) {
        static_cast<void>(device_.destroy(font_));
    }
    if (set_.valid()) {
        static_cast<void>(device_.destroy(set_));
    }
    if (pipeline_.valid()) {
        static_cast<void>(device_.destroy(pipeline_));
    }
    if (layout_.valid()) {
        static_cast<void>(device_.destroy(layout_));
    }
    if (fence_.valid()) {
        static_cast<void>(device_.destroy(fence_));
    }
}

void ImGuiRenderer::destroy_buffers() {
    for (rhi::BufferHandle* buffer : {&vertex_, &index_, &staging_vertex_, &staging_index_}) {
        if (buffer->valid()) {
            static_cast<void>(device_.destroy(*buffer));
            *buffer = rhi::BufferHandle{};
        }
    }
    capacity_ = 0;
    vertex_state_ = ResourceState::Common;
    index_state_ = ResourceState::Common;
    index_fill_pending_ = false;
}

RhiStatus ImGuiRenderer::upload_font_atlas(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height) {
    if (pixels == nullptr || width == 0 || height == 0) {
        return rhi::fail<core::Unit>(rhi::RhiErrorCode::InvalidDescriptor, "imgui: the font atlas needs pixels and a size");
    }
    // The rows are copied to the device's row pitch; the atlas itself is tightly packed RGBA8.
    const std::uint64_t pitch = rhi::copy_row_pitch(width, Format::R8G8B8A8Unorm);
    const std::uint64_t row = static_cast<std::uint64_t>(width) * 4;
    std::vector<std::uint8_t> packed(pitch * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(packed.data() + y * pitch, pixels + y * row, row);
    }

    auto texture = device_.create_texture(rhi::TextureDesc{width, height, 1, Format::R8G8B8A8Unorm,
                                                           rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDestination,
                                                           "imgui font atlas"});
    if (!texture.ok()) {
        return core::failure(texture.error());
    }
    rhi::TextureHandle font = std::move(texture).value();

    auto staging = device_.create_buffer(BufferDesc{packed.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "imgui font staging"});
    if (!staging.ok()) {
        static_cast<void>(device_.destroy(font));
        return core::failure(staging.error());
    }
    rhi::BufferHandle staging_buffer = std::move(staging).value();
    if (RhiStatus uploaded = device_.upload_buffer(staging_buffer, 0, packed.size(), packed.data()); !uploaded.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        static_cast<void>(device_.destroy(font));
        return uploaded;
    }

    auto list_result = device_.create_command_list(QueueType::Graphics);
    if (!list_result.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        static_cast<void>(device_.destroy(font));
        return core::failure(list_result.error());
    }
    std::unique_ptr<CommandList> list = std::move(list_result).value();
    RhiStatus recorded = list->transition(font, ResourceState::Common, ResourceState::CopyDestination);
    if (recorded.ok()) {
        recorded = list->copy_buffer_to_texture(staging_buffer, 0, font);
    }
    if (recorded.ok()) {
        recorded = list->transition(font, ResourceState::CopyDestination, ResourceState::ShaderRead);
    }
    if (recorded.ok()) {
        recorded = list->close();
    }
    if (recorded.ok()) {
        const std::vector<CommandList*> lists{list.get()};
        recorded = device_.submit(QueueType::Graphics, lists, fence_, ++fence_value_);
    }
    if (recorded.ok()) {
        // The upload is one-time and small, so the call waits for it and the staging buffer can go at once.
        recorded = device_.wait_fence(fence_, fence_value_);
    }
    static_cast<void>(device_.destroy(staging_buffer));
    if (!recorded.ok()) {
        static_cast<void>(device_.destroy(font));
        return recorded;
    }

    if (RhiStatus written = device_.write_texture(set_, 0, 0, font); !written.ok()) {
        static_cast<void>(device_.destroy(font));
        return written;
    }
    if (font_.valid()) {
        static_cast<void>(device_.destroy(font_));
    }
    font_ = font;
    return core::success();
}

RhiStatus ImGuiRenderer::ensure_capacity(std::uint32_t vertex_count) {
    if (vertex_count <= capacity_ && capacity_ != 0) {
        return core::success();
    }
    const std::uint32_t capacity = std::max({vertex_count, capacity_ * 2, kMinimumVertexCapacity});
    destroy_buffers();

    const std::uint64_t vertex_bytes = capacity * kVertexBytes;
    const std::uint64_t index_bytes = capacity * kIndexBytes;
    auto vertex = device_.create_buffer(BufferDesc{vertex_bytes, BufferUsage::Vertex | BufferUsage::CopyDestination, "imgui vertices"});
    if (!vertex.ok()) {
        return core::failure(vertex.error());
    }
    vertex_ = std::move(vertex).value();
    auto staging_v = device_.create_buffer(BufferDesc{vertex_bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "imgui vertex staging"});
    if (!staging_v.ok()) {
        return core::failure(staging_v.error());
    }
    staging_vertex_ = std::move(staging_v).value();
    auto index = device_.create_buffer(BufferDesc{index_bytes, BufferUsage::Index | BufferUsage::CopyDestination, "imgui indices"});
    if (!index.ok()) {
        return core::failure(index.error());
    }
    index_ = std::move(index).value();
    auto staging_i = device_.create_buffer(BufferDesc{index_bytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "imgui index staging"});
    if (!staging_i.ok()) {
        return core::failure(staging_i.error());
    }
    staging_index_ = std::move(staging_i).value();

    // Vertices are expanded one per index, so the index buffer is 0, 1, 2, ... and is filled once per growth.
    std::vector<std::uint32_t> sequence(capacity);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        sequence[i] = i;
    }
    if (RhiStatus filled = device_.upload_buffer(staging_index_, 0, index_bytes, sequence.data()); !filled.ok()) {
        return filled;
    }
    capacity_ = capacity;
    index_fill_pending_ = true;
    return core::success();
}

RhiStatus ImGuiRenderer::record(CommandList& list, rhi::TextureHandle target, const ImDrawData& draw_data,
                                const std::array<float, 4>& clear_color) {
    if (!font_.valid()) {
        return rhi::fail<core::Unit>(rhi::RhiErrorCode::InvalidState, "imgui: the font atlas must be uploaded before a frame is recorded");
    }

    // Expand every draw into one vertex per index. Each vertex carries its command's clip rectangle, in framebuffer pixels.
    const float width = draw_data.DisplaySize.x;
    const float height = draw_data.DisplaySize.y;
    std::vector<ImGuiVertex> vertices;
    if (width > 0.0f && height > 0.0f) {
        const float origin_x = draw_data.DisplayPos.x;
        const float origin_y = draw_data.DisplayPos.y;
        for (int n = 0; n < draw_data.CmdLists.Size; ++n) {
            const ImDrawList* draw_list = draw_data.CmdLists[n];
            for (const ImDrawCmd& cmd : draw_list->CmdBuffer) {
                // The editor issues no user callbacks, so a command with one carries no geometry for this renderer.
                if (cmd.UserCallback != nullptr || cmd.ElemCount == 0) {
                    continue;
                }
                const float x0 = std::max(0.0f, cmd.ClipRect.x - origin_x);
                const float y0 = std::max(0.0f, cmd.ClipRect.y - origin_y);
                const float x1 = std::min(width, cmd.ClipRect.z - origin_x);
                const float y1 = std::min(height, cmd.ClipRect.w - origin_y);
                if (x1 <= x0 || y1 <= y0) {
                    continue;
                }
                for (unsigned int i = 0; i < cmd.ElemCount; ++i) {
                    const std::size_t index = static_cast<std::size_t>(draw_list->IdxBuffer.Data[cmd.IdxOffset + i]) + cmd.VtxOffset;
                    const ImDrawVert& vert = draw_list->VtxBuffer.Data[index];
                    const float px = vert.pos.x - origin_x;
                    const float py = vert.pos.y - origin_y;
                    const float ndc_x = 2.0f * px / width - 1.0f;
                    const float ndc_y = y_up_ ? 1.0f - 2.0f * py / height : 2.0f * py / height - 1.0f;
                    vertices.push_back(ImGuiVertex{ndc_x, ndc_y, vert.uv.x, vert.uv.y, vert.col, x0, y0, x1, y1});
                }
            }
        }
    }
    last_vertex_count_ = static_cast<std::uint32_t>(vertices.size());

    if (!vertices.empty()) {
        if (RhiStatus ready = ensure_capacity(last_vertex_count_); !ready.ok()) {
            return ready;
        }
        const std::uint64_t vertex_bytes = vertices.size() * kVertexBytes;
        if (RhiStatus uploaded = device_.upload_buffer(staging_vertex_, 0, vertex_bytes, vertices.data()); !uploaded.ok()) {
            return uploaded;
        }
        RhiStatus recorded = list.transition(vertex_, vertex_state_, ResourceState::CopyDestination);
        if (recorded.ok()) {
            recorded = list.copy_buffer(staging_vertex_, 0, vertex_, 0, vertex_bytes);
        }
        if (recorded.ok()) {
            recorded = list.transition(vertex_, ResourceState::CopyDestination, ResourceState::VertexBuffer);
        }
        if (!recorded.ok()) {
            return recorded;
        }
        vertex_state_ = ResourceState::VertexBuffer;

        if (index_fill_pending_) {
            const std::uint64_t index_bytes = static_cast<std::uint64_t>(capacity_) * kIndexBytes;
            recorded = list.transition(index_, index_state_, ResourceState::CopyDestination);
            if (recorded.ok()) {
                recorded = list.copy_buffer(staging_index_, 0, index_, 0, index_bytes);
            }
            if (recorded.ok()) {
                recorded = list.transition(index_, ResourceState::CopyDestination, ResourceState::IndexBuffer);
            }
            if (!recorded.ok()) {
                return recorded;
            }
            index_state_ = ResourceState::IndexBuffer;
            index_fill_pending_ = false;
        }
    }

    rhi::RenderPassDesc pass;
    pass.color_targets = {target};
    pass.clear_color = clear_color;
    if (RhiStatus began = list.begin_render_pass(pass); !began.ok()) {
        return began;
    }
    if (!vertices.empty()) {
        static_cast<void>(list.set_pipeline(pipeline_));
        static_cast<void>(list.set_descriptor_set(0, set_));
        static_cast<void>(list.set_vertex_buffer(0, vertex_));
        static_cast<void>(list.set_index_buffer(index_, rhi::IndexFormat::Uint32));
        if (RhiStatus drawn = list.draw_indexed(last_vertex_count_, 1, 0, 0, 0); !drawn.ok()) {
            static_cast<void>(list.end_render_pass());
            return drawn;
        }
    }
    return list.end_render_pass();
}

}  // namespace engine::editor
