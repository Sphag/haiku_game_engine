// The editor's ImGui renderer on a real device (US-061): a frame of filled rectangles, a clipped rectangle, and Cyrillic text is drawn
// through engine_rhi into a target, read back, and checked pixel by pixel. Two frames run, so the vertex buffer's state handoff between
// frames is exercised. Run with --suite editor --backend dx12|vulkan; the CPU editor tests cover everything else.

#include "editor_fixture.hpp"
#include "gpu_test_options.hpp"

#include "engine/editor/imgui_renderer.hpp"
#include "engine/rhi/device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using engine::editor::ImGuiRenderer;
using engine::editor::ImGuiShaders;
using engine::rhi::Backend;
using engine::rhi::BufferDesc;
using engine::rhi::BufferUsage;
using engine::rhi::CommandList;
using engine::rhi::DeviceDesc;
using engine::rhi::Format;
using engine::rhi::QueueType;
using engine::rhi::ResourceState;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::TextureDesc;
using engine::rhi::TextureUsage;

constexpr std::uint32_t kSize = 256;

template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

void require_ok(const RhiStatus& status) {
    if (!status.ok()) {
        FAIL(status.error().message);
    }
}

DeviceDesc debug_device(Backend backend) {
    DeviceDesc desc;
    desc.backend = backend;
    desc.debug_layers = true;
    desc.allow_software_adapter = engine::tests::allow_software_adapters();
    return desc;
}

std::vector<std::uint8_t> load_shader(Backend backend, const std::string& name) {
    const std::string path = std::string(ENGINE_TEST_SHADER_DIR) + "/" + name + (backend == Backend::Dx12 ? ".dxil" : ".spv");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FAIL("cannot open shader " << path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

ImGuiShaders load_editor_shaders(Backend backend) {
    ImGuiShaders shaders;
    shaders.vertex = engine::rhi::ShaderBinary{engine::rhi::ShaderStage::Vertex, "VSMain", load_shader(backend, "editor_imgui_vertex")};
    shaders.pixel = engine::rhi::ShaderBinary{engine::rhi::ShaderStage::Pixel, "PSMain", load_shader(backend, "editor_imgui_pixel")};
    return shaders;
}

// Builds one frame: a red block, a green block that a clip rectangle cuts to its left half, and a line of Cyrillic text.
// The flags let a diagnostic draw the parts alone.
// The foreground list is drawn after every window, so the shapes are never covered by a window.
void build_frame(bool rectangles, bool text) {
    ImGui::NewFrame();
    // A window that draws nothing: it stops ImGui from drawing its fallback window over the target.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kSize), static_cast<float>(kSize)));
    ImGui::Begin("editor test", nullptr,
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
    ImGui::End();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (rectangles) {
        draw->AddRectFilled(ImVec2(10.0f, 10.0f), ImVec2(50.0f, 50.0f), IM_COL32(255, 0, 0, 255));
        draw->PushClipRect(ImVec2(100.0f, 100.0f), ImVec2(120.0f, 120.0f), false);
        draw->AddRectFilled(ImVec2(100.0f, 100.0f), ImVec2(160.0f, 160.0f), IM_COL32(0, 255, 0, 255));
        draw->PopClipRect();
    }
    if (text) {
        draw->AddText(ImGui::GetFont(), 24.0f, ImVec2(10.0f, 180.0f), IM_COL32(255, 255, 255, 255), "Проект");
    }
    ImGui::Render();
}

// `before` is the state the target was left in: Common for a new texture, CopySource after the previous frame's readback.
std::vector<std::uint8_t> render_and_read(engine::rhi::Device& device, ImGuiRenderer& renderer, engine::rhi::TextureHandle target,
                                          ResourceState before, engine::rhi::BufferHandle readback, std::uint64_t readback_bytes,
                                          engine::rhi::FenceHandle fence, std::uint64_t value) {
    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(target, before, ResourceState::RenderTarget));
    require_ok(renderer.record(*list, target, *ImGui::GetDrawData(), {0.0f, 0.0f, 0.0f, 1.0f}));
    require_ok(list->transition(target, ResourceState::RenderTarget, ResourceState::CopySource));
    require_ok(list->copy_texture_to_buffer(target, readback, 0));
    require_ok(list->close());
    std::vector<CommandList*> lists{list.get()};
    require_ok(device.submit(QueueType::Graphics, lists, fence, value));
    require_ok(device.wait_fence(fence, value));

    std::vector<std::uint8_t> bytes(readback_bytes);
    require_ok(device.read_buffer(readback, 0, readback_bytes, bytes.data()));
    return bytes;
}

void run_editor_frame_case(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto renderer = take(ImGuiRenderer::create(*device, Format::R8G8B8A8Unorm, load_editor_shaders(backend)));

    engine::tests::editor_fixture::ImGuiScope scope(true);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kSize), static_cast<float>(kSize));
    unsigned char* atlas = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
    require_ok(renderer->upload_font_atlas(atlas, static_cast<std::uint32_t>(atlas_width), static_cast<std::uint32_t>(atlas_height)));

    auto target = take(device->create_texture(TextureDesc{kSize, kSize, 1, Format::R8G8B8A8Unorm,
                                                          TextureUsage::RenderTarget | TextureUsage::CopySource, "editor test target"}));
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(kSize, Format::R8G8B8A8Unorm);
    const std::uint64_t readback_bytes = pitch * kSize;
    auto readback = take(device->create_buffer(BufferDesc{readback_bytes, BufferUsage::CpuRead | BufferUsage::CopyDestination, "editor readback"}));
    auto fence = take(device->create_fence(0));

    // Each variant is one frame. The target's state carries over: after a readback it is in CopySource.
    struct Variant {
        const char* name;
        bool rectangles;
        bool text;
    };
    ResourceState before = ResourceState::Common;
    std::uint64_t fence_value = 0;
    for (const Variant variant : {Variant{"rectangles", true, false}, Variant{"text", false, true}, Variant{"both", true, true}}) {
        INFO(variant.name);
        build_frame(variant.rectangles, variant.text);
        const auto bytes = render_and_read(*device, *renderer, target, before, readback, readback_bytes, fence, ++fence_value);
        before = ResourceState::CopySource;
        REQUIRE(bytes.size() == readback_bytes);
        CHECK(renderer->last_vertex_count() > 0);

        auto channel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t c) {
            return bytes[y * pitch + x * 4 + c];
        };
        if (variant.rectangles) {
            // The red block is drawn; the green block shows inside its clip rectangle and not past it.
            CHECK(channel(30, 30, 0) > 200);
            CHECK(channel(30, 30, 1) < 40);
            CHECK(channel(110, 110, 1) > 200);
            CHECK(channel(150, 150, 1) < 40);
        }
        if (variant.text) {
            // The white text lands in its band, and nothing is drawn above the band.
            std::size_t bright = 0;
            for (std::uint32_t y = 180; y < 210; ++y) {
                for (std::uint32_t x = 10; x < 120; ++x) {
                    bright += channel(x, y, 0) > 128 && channel(x, y, 1) > 128 ? 1U : 0U;
                }
            }
            CHECK(bright > 50);
            CHECK(channel(30, 30, 1) < 40);
        }
    }

    require_ok(device->check_debug_messages());
    require_ok(device->wait_idle());
    static_cast<void>(device->destroy(readback));
    static_cast<void>(device->destroy(target));
    static_cast<void>(device->destroy(fence));
}

}  // namespace

TEST_CASE("DX12 editor UI: rectangles, a clip rectangle, and Cyrillic text draw through the RHI", "[gpu][editor][dx12]") {
    run_editor_frame_case(Backend::Dx12);
}

TEST_CASE("Vulkan editor UI: rectangles, a clip rectangle, and Cyrillic text draw through the RHI", "[gpu][editor][vulkan]") {
    run_editor_frame_case(Backend::Vulkan);
}
