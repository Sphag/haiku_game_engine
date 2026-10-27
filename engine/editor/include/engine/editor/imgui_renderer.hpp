#pragma once

// Dear ImGui drawn through the RHI (US-061). One renderer serves both graphics backends: it records through engine_rhi and never
// touches a native API, so the vendored DX12 and Vulkan backends are not used. The draw data is expanded on the CPU into one
// vertex per index, each carrying its command's clip rectangle, so a single indexed draw covers the whole frame.
//
// Frame flow: the caller transitions the target to RenderTarget, records with record(), transitions it back to Present, and submits.
// The renderer copies vertices through a staging buffer each frame, so the caller waits for each frame's fence before the next record.

#include "engine/rhi/device.hpp"
#include "engine/rhi/error.hpp"
#include "engine/rhi/types.hpp"

#include <array>
#include <cstdint>
#include <memory>

struct ImDrawData;

namespace engine::editor {

// The compiled shaders of imgui.hlsl: the vertex entry VSMain and the pixel entry PSMain.
struct ImGuiShaders {
    rhi::ShaderBinary vertex;
    rhi::ShaderBinary pixel;
};

// One expanded vertex, 36 bytes. imgui.hlsl reads the same layout; the attributes are in ImGuiRenderer::create. The color is
// the ImGui RGBA8 word, passed as a uint because the RHI has no 8-bit vertex format.
struct ImGuiVertex {
    float x = 0.0f;  // NDC
    float y = 0.0f;  // NDC, up on DX12 and down on Vulkan (see the renderer)
    float u = 0.0f;  // font atlas coordinates, normalized
    float v = 0.0f;
    std::uint32_t color = 0;  // RGBA8 bytes in memory order
    float clip_x0 = 0.0f;    // clip rectangle in framebuffer pixels
    float clip_y0 = 0.0f;
    float clip_x1 = 0.0f;
    float clip_y1 = 0.0f;
};
static_assert(sizeof(ImGuiVertex) == 36, "imgui.hlsl reads a 36-byte vertex");

class ImGuiRenderer {
public:
    // Creates the pipeline for one target format. The font atlas is uploaded separately, once the atlas is built.
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<ImGuiRenderer>> create(rhi::Device& device, rhi::Format target_format,
                                                                               const ImGuiShaders& shaders);
    // The renderer's handles must be destroyed before the device. The destructor destroys them and ignores failures.
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    ImGuiRenderer(ImGuiRenderer&&) = delete;
    ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;

    // Uploads RGBA8 font atlas pixels (width x height, rows top to bottom) and waits until the GPU has them. A second upload
    // replaces the first.
    [[nodiscard]] rhi::RhiStatus upload_font_atlas(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height);

    // Records one render pass over `target`, which must be in RenderTarget state. The target is cleared to `clear_color`, then
    // the draw data is drawn over it. The caller submits the list.
    [[nodiscard]] rhi::RhiStatus record(rhi::CommandList& list, rhi::TextureHandle target, const ImDrawData& draw_data,
                                        const std::array<float, 4>& clear_color);

    // Vertices drawn by the last record(). Tests use it to check the expansion.
    [[nodiscard]] std::uint32_t last_vertex_count() const noexcept {
        return last_vertex_count_;
    }

private:
    ImGuiRenderer(rhi::Device& device, bool y_up);

    rhi::RhiStatus ensure_capacity(std::uint32_t vertex_count);
    void destroy_buffers();

    rhi::Device& device_;
    bool y_up_;  // NDC y is up on DX12 and down on Vulkan; the expansion writes the sign the backend reads
    rhi::DescriptorSetLayoutHandle layout_;
    rhi::DescriptorSetHandle set_;
    rhi::PipelineHandle pipeline_;
    rhi::FenceHandle fence_;
    std::uint64_t fence_value_ = 0;

    rhi::TextureHandle font_;

    std::uint32_t capacity_ = 0;  // vertices the device buffers hold
    rhi::BufferHandle vertex_;
    rhi::BufferHandle index_;
    rhi::BufferHandle staging_vertex_;
    rhi::BufferHandle staging_index_;
    rhi::ResourceState vertex_state_ = rhi::ResourceState::Common;
    rhi::ResourceState index_state_ = rhi::ResourceState::Common;
    bool index_fill_pending_ = false;  // a grown index buffer needs its sequential indices copied in

    std::uint32_t last_vertex_count_ = 0;
};

}  // namespace engine::editor
