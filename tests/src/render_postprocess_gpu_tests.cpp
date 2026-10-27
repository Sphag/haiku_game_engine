// HDR post pass on real GPUs (US-039). Each case runs PostProcessor (TAA, bloom, tone map, overlay) on DX12 and on Vulkan with the debug
// layers on, and compares the displayed image with the CPU reference in postprocess.hpp, pixel by pixel. Any validation error fails the
// check. The cases cover: NaN, Inf, and negative inputs; the reference overlay against two exposures; the history of a moving block, and its
// reset by a camera cut, a teleport, and a resize; and the scene renderer with the post pass on.
#include "render_scene_fixture.hpp"

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/material.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/post_processor.hpp"
#include "engine/render/postprocess.hpp"
#include "engine/render/render_graph.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"

#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using engine::render::Access;
using engine::render::bloom_blur_weights;
using engine::render::bloom_prefilter;
using engine::render::CompileResult;
using engine::render::composite_overlay;
using engine::render::exposure_from_ev100;
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::HistoryReset;
using engine::render::ImageRgba8;
using engine::render::kBloomRadius;
using engine::render::pick_entity;
using engine::render::PassContext;
using engine::render::PassKind;
using engine::render::PostInputs;
using engine::render::PostProcessor;
using engine::render::PostProcessSettings;
using engine::render::PostShaders;
using engine::render::postprocess_pixel;
using engine::render::RenderGraph;
using engine::render::ResourceId;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::srgb_encode;
using engine::render::TaaFrameInput;
using engine::render::TaaHistory;
using engine::rhi::Backend;
using engine::rhi::BufferDesc;
using engine::rhi::BufferUsage;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::Format;
using engine::rhi::ResourceState;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::TextureDesc;
using engine::rhi::TextureUsage;
using engine::render::tests::build_golden_scene;
using engine::render::tests::GoldenScene;
using engine::render::tests::kSceneHeight;
using engine::render::tests::kSceneWidth;

namespace {

// Tolerance of the byte comparisons. The GPU stores half floats in the history and bloom targets, so a value near 1 carries about 1e-3 of
// rounding, and the 8-bit output adds half a step.
constexpr int kByteTolerance = 3;

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

PostShaders post_shaders(Backend backend) {
    PostShaders shaders;
    shaders.taa = ShaderBinary{ShaderStage::Compute, "CSTaa", load_shader(backend, "post_taa")};
    shaders.bloom_horizontal = ShaderBinary{ShaderStage::Compute, "CSBloomHorizontal", load_shader(backend, "post_bloom_horizontal")};
    shaders.bloom_vertical = ShaderBinary{ShaderStage::Compute, "CSBloomVertical", load_shader(backend, "post_bloom_vertical")};
    shaders.tonemap = ShaderBinary{ShaderStage::Compute, "CSTonemap", load_shader(backend, "post_tonemap")};
    return shaders;
}

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

TextureDesc texture_desc(std::uint32_t width, std::uint32_t height, Format format, TextureUsage usage, const char* name) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

// Copies `bytes` (rows at the copy pitch) into a texture that is in Common, and leaves it in Common.
void upload_texture(Device& device, engine::rhi::TextureHandle texture, const TextureDesc& desc, const std::vector<std::uint8_t>& bytes) {
    const BufferDesc staging_desc = buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "test staging");
    engine::rhi::BufferHandle staging = take(device.create_buffer(staging_desc));
    require_ok(device.upload_buffer(staging, 0, bytes.size(), bytes.data()));
    RenderGraph graph;
    const ResourceId staging_res = graph.import_buffer("staging", staging, staging_desc, ResourceState::CopySource, ResourceState::CopySource);
    const ResourceId texture_res = graph.import_texture("texture", texture, desc, ResourceState::Common, ResourceState::Common);
    graph.add_pass("upload", PassKind::Compute, {{staging_res, Access::CopySource}, {texture_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_buffer_to_texture(ctx.buffer(staging_res), 0, ctx.texture(texture_res)); });
    const CompileResult compiled = graph.compile();
    REQUIRE(compiled.ok());
    const auto executed = engine::render::execute(device, *compiled.graph);
    static_cast<void>(device.destroy(staging));
    if (!executed.ok()) {
        FAIL(executed.error().message);
    }
}

// One frame's inputs: the HDR color and the motion vectors, and a readback buffer for the displayed image.
struct TestFrame {
    engine::rhi::TextureHandle color;
    engine::rhi::TextureHandle velocity;
    engine::rhi::BufferHandle readback;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

TestFrame make_frame(Device& device, std::uint32_t width, std::uint32_t height, const std::vector<glm::vec3>& hdr,
                     const std::vector<glm::vec2>& motion) {
    TestFrame frame;
    frame.width = width;
    frame.height = height;
    const TextureDesc color_desc =
        texture_desc(width, height, Format::R16G16B16A16Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "test color");
    const TextureDesc velocity_desc =
        texture_desc(width, height, Format::R32G32Float, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "test velocity");
    frame.color = take(device.create_texture(color_desc));
    frame.velocity = take(device.create_texture(velocity_desc));
    const std::uint64_t readback_bytes = engine::rhi::copy_row_pitch(width, Format::R8G8B8A8Unorm) * height;
    frame.readback = take(device.create_buffer(buffer_desc(readback_bytes, BufferUsage::CpuRead | BufferUsage::CopyDestination, "test readback")));

    const std::uint64_t color_pitch = engine::rhi::copy_row_pitch(width, Format::R16G16B16A16Float);
    std::vector<std::uint8_t> color_bytes(color_pitch * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const glm::vec3 c = hdr[static_cast<std::size_t>(y) * width + x];
            const std::uint16_t halves[4] = {glm::packHalf1x16(c.x), glm::packHalf1x16(c.y), glm::packHalf1x16(c.z), glm::packHalf1x16(1.0f)};
            std::memcpy(color_bytes.data() + y * color_pitch + x * 8, halves, sizeof(halves));
        }
    }
    upload_texture(device, frame.color, color_desc, color_bytes);

    const std::uint64_t velocity_pitch = engine::rhi::copy_row_pitch(width, Format::R32G32Float);
    std::vector<std::uint8_t> velocity_bytes(velocity_pitch * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const glm::vec2 v = motion[static_cast<std::size_t>(y) * width + x];
            std::memcpy(velocity_bytes.data() + y * velocity_pitch + x * 8, &v, sizeof(v));
        }
    }
    upload_texture(device, frame.velocity, velocity_desc, velocity_bytes);
    return frame;
}

void destroy_frame(Device& device, TestFrame& frame) {
    static_cast<void>(device.destroy(frame.color));
    static_cast<void>(device.destroy(frame.velocity));
    static_cast<void>(device.destroy(frame.readback));
}

// The displayed image, row 0 first. Each entry is RGBA8.
std::vector<std::array<std::uint8_t, 4>> read_display(Device& device, const TestFrame& frame) {
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(frame.width, Format::R8G8B8A8Unorm);
    std::vector<std::uint8_t> bytes(pitch * frame.height);
    require_ok(device.read_buffer(frame.readback, 0, bytes.size(), bytes.data()));
    std::vector<std::array<std::uint8_t, 4>> out(static_cast<std::size_t>(frame.width) * frame.height);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            std::memcpy(out[static_cast<std::size_t>(y) * frame.width + x].data(), bytes.data() + y * pitch + x * 4, 4);
        }
    }
    return out;
}

// CPU reference of the displayed image for resolved HDR colors (postprocess.hpp): bloom from the prefiltered colors, blurred with clamped
// edges (separable, as the GPU does it), exposure and bloom in linear light, the ACES curve, the sRGB encode, and the overlay last.
std::vector<glm::vec3> expected_display(const std::vector<glm::vec3>& resolved, std::uint32_t width, std::uint32_t height,
                                        const PostProcessSettings& settings, const std::vector<glm::vec4>* overlay) {
    const std::size_t count = resolved.size();
    const float exposure = exposure_from_ev100(settings.ev100 + settings.exposure.ev_compensation);
    const std::vector<float> weights = bloom_blur_weights(kBloomRadius);
    const auto radius = static_cast<int>(kBloomRadius);
    const auto w = static_cast<int>(width);
    const auto h = static_cast<int>(height);
    const auto at = [w](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };

    std::vector<glm::vec3> prefiltered(count);
    for (std::size_t i = 0; i < count; ++i) {
        prefiltered[i] = bloom_prefilter(resolved[i], settings.bloom);
    }
    std::vector<glm::vec3> horizontal(count, glm::vec3(0.0f));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int i = -radius; i <= radius; ++i) {
                const int xi = std::clamp(x + i, 0, w - 1);
                horizontal[at(x, y)] += weights[static_cast<std::size_t>(i + radius)] * prefiltered[at(xi, y)];
            }
        }
    }
    std::vector<glm::vec3> out(count);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            glm::vec3 bloom(0.0f);
            for (int j = -radius; j <= radius; ++j) {
                const int yj = std::clamp(y + j, 0, h - 1);
                bloom += weights[static_cast<std::size_t>(j + radius)] * horizontal[at(x, yj)];
            }
            glm::vec3 display = srgb_encode(postprocess_pixel(resolved[at(x, y)], bloom, exposure, settings.bloom.intensity));
            if (overlay != nullptr) {
                display = composite_overlay(display, (*overlay)[at(x, y)]);
            }
            out[at(x, y)] = display;
        }
    }
    return out;
}

int to_byte(float value) {
    return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

// Compares the displayed image with the CPU reference. Returns how many pixels differ by more than the tolerance; the first one is reported.
int count_mismatches(const std::vector<std::array<std::uint8_t, 4>>& gpu, const std::vector<glm::vec3>& expected, std::uint32_t width) {
    int mismatches = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            const auto channel = static_cast<std::size_t>(c);
            const int diff = std::abs(static_cast<int>(gpu[i][channel]) - to_byte(expected[i][c]));
            if (diff > kByteTolerance) {
                if (mismatches == 0) {
                    INFO("first mismatch at pixel (" << i % width << ", " << i / width << ") channel " << c << ": gpu "
                                                     << static_cast<int>(gpu[i][channel]) << " expected " << to_byte(expected[i][c]));
                }
                ++mismatches;
                break;
            }
        }
    }
    return mismatches;
}

PostProcessSettings test_settings() {
    PostProcessSettings settings;
    settings.enabled = true;
    settings.taa = true;
    settings.ev100 = 0.0f;
    settings.bloom.threshold = 1.0f;
    settings.bloom.knee = 0.5f;
    settings.bloom.intensity = 0.1f;
    return settings;
}

void create_post(Device& device, Backend backend, std::uint32_t width, std::uint32_t height, const PostProcessSettings& settings,
                 std::unique_ptr<PostProcessor>& out) {
    out = take(PostProcessor::create(device, width, height, settings, post_shaders(backend)));
}

// A HDR frame with NaN, Inf, and negative samples next to a bright pixel. Every sample is sanitized before it is filtered.
void run_non_finite_case(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const std::uint32_t w = 16;
    const std::uint32_t h = 16;
    std::vector<glm::vec3> hdr(static_cast<std::size_t>(w) * h, glm::vec3(0.5f));
    hdr[3 + 3 * w] = glm::vec3(std::nanf(""), 1.0f, 2.0f);
    hdr[4 + 3 * w] = glm::vec3(std::numeric_limits<float>::infinity(), 0.5f, 0.5f);
    hdr[5 + 3 * w] = glm::vec3(-2.0f, -1.0f, 0.25f);
    hdr[8 + 8 * w] = glm::vec3(6.0f);
    const std::vector<glm::vec2> motion(hdr.size(), glm::vec2(0.0f));

    std::unique_ptr<PostProcessor> post;
    create_post(*device, backend, w, h, test_settings(), post);
    TestFrame frame = make_frame(*device, w, h, hdr, motion);
    require_ok(post->run(PostInputs{frame.color, frame.velocity, frame.readback, glm::dvec3(0.0), false}));
    CHECK(post->last_reset() == HistoryReset::FirstFrame);

    // The first frame keeps only its current color: the sanitized input (NaN and Inf to 0, negatives to 0).
    std::vector<glm::vec3> resolved(hdr.size());
    for (std::size_t i = 0; i < hdr.size(); ++i) {
        resolved[i] = engine::render::sanitize_color(hdr[i]);
    }
    CHECK(count_mismatches(read_display(*device, frame), expected_display(resolved, w, h, test_settings(), nullptr), w) == 0);
    require_ok(device->check_debug_messages());

    require_ok(post->release());
    destroy_frame(*device, frame);
    require_ok(device->check_debug_messages());
}

// The overlay is composited after tone mapping, so two exposures show the same overlay colors. The left half is opaque, the right half
// translucent over the scene.
void run_overlay_case(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const std::uint32_t w = 16;
    const std::uint32_t h = 16;
    const std::vector<glm::vec3> hdr(static_cast<std::size_t>(w) * h, glm::vec3(0.3f));
    const std::vector<glm::vec2> motion(hdr.size(), glm::vec2(0.0f));
    ImageRgba8 overlay;
    overlay.width = w;
    overlay.height = h;
    overlay.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    std::vector<glm::vec4> overlay_float(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const bool left = x < w / 2;
            const std::array<std::uint8_t, 4> texel = left ? std::array<std::uint8_t, 4>{50, 150, 230, 255}
                                                           : std::array<std::uint8_t, 4>{200, 20, 20, 128};
            std::memcpy(overlay.pixels.data() + (static_cast<std::size_t>(y) * w + x) * 4, texel.data(), 4);
            overlay_float[static_cast<std::size_t>(y) * w + x] = glm::vec4(texel[0], texel[1], texel[2], texel[3]) / 255.0f;
        }
    }

    std::unique_ptr<PostProcessor> post;
    create_post(*device, backend, w, h, test_settings(), post);
    require_ok(post->set_overlay(overlay));
    TestFrame frame = make_frame(*device, w, h, hdr, motion);
    for (const float ev100 : {-2.0f, 3.0f}) {
        INFO("exposure EV100 " << ev100);
        PostProcessSettings settings = test_settings();
        settings.ev100 = ev100;
        post->set_settings(settings);
        post->reset_history();
        require_ok(post->run(PostInputs{frame.color, frame.velocity, frame.readback, glm::dvec3(0.0), false}));
        const auto display = read_display(*device, frame);
        CHECK(count_mismatches(display, expected_display(hdr, w, h, settings, &overlay_float), w) == 0);
        // The opaque half shows the overlay color itself, whatever the exposure.
        CHECK(std::abs(static_cast<int>(display[0][0]) - 50) <= 1);
        CHECK(std::abs(static_cast<int>(display[0][1]) - 150) <= 1);
        CHECK(std::abs(static_cast<int>(display[0][2]) - 230) <= 1);
    }
    require_ok(post->release());
    destroy_frame(*device, frame);
    require_ok(device->check_debug_messages());
}

// A 4 x 4 block moves 2 pixels right each frame and gets brighter. The GPU history follows it (the motion vectors say where each pixel came
// from), and the CPU TaaHistory with the same inputs gives the same image. A camera cut, a teleport, and a resize each discard the history.
void run_history_case(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const std::uint32_t w = 32;
    const std::uint32_t h = 32;
    const PostProcessSettings settings = test_settings();

    std::unique_ptr<PostProcessor> post;
    create_post(*device, backend, w, h, settings, post);
    TaaHistory cpu;
    glm::dvec3 camera(0.0);

    // The block starts at x = 4 + 2 * frame, and its brightness grows each frame, so its history differs from its current value.
    const auto make_scene = [&](int frame_index, std::vector<glm::vec3>& hdr, std::vector<glm::vec2>& motion) {
        hdr.assign(static_cast<std::size_t>(w) * h, glm::vec3(0.1f));
        motion.assign(hdr.size(), glm::vec2(0.0f));
        const int x0 = 4 + 2 * frame_index;
        const float brightness = 0.2f + 0.2f * static_cast<float>(frame_index);
        for (int y = 12; y < 16; ++y) {
            for (int x = x0; x < x0 + 4; ++x) {
                hdr[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = glm::vec3(brightness);
                // Motion in UV units: the block moved 2 pixels right since the last frame, so each pixel came from 2 pixels to its left.
                motion[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = glm::vec2(2.0f / static_cast<float>(w), 0.0f);
            }
        }
    };

    struct Step {
        int frame_index;
        bool camera_cut;
        double camera_x;  // the camera's x position this frame
        HistoryReset expected_reset;
    };
    const std::vector<Step> steps{
        {0, false, 0.0, HistoryReset::FirstFrame}, {1, false, 0.0, HistoryReset::None},       {2, false, 0.0, HistoryReset::None},
        {3, false, 0.0, HistoryReset::None},        {4, false, 0.0, HistoryReset::None},       {5, true, 0.0, HistoryReset::CameraCut},
        {6, false, 80.0, HistoryReset::Teleport},
    };
    for (const Step& step : steps) {
        INFO("frame " << step.frame_index);
        std::vector<glm::vec3> hdr;
        std::vector<glm::vec2> motion;
        make_scene(step.frame_index, hdr, motion);
        camera.x = step.camera_x;
        TestFrame frame = make_frame(*device, w, h, hdr, motion);
        require_ok(post->run(PostInputs{frame.color, frame.velocity, frame.readback, camera, step.camera_cut}));
        CHECK(post->last_reset() == step.expected_reset);

        TaaFrameInput cpu_input;
        cpu_input.width = w;
        cpu_input.height = h;
        cpu_input.camera_cut = step.camera_cut;
        cpu_input.teleport = step.camera_x > 50.0;
        static_cast<void>(cpu.begin_frame(cpu_input));
        const std::vector<glm::vec3> resolved = cpu.resolve(hdr, motion, settings.taa_settings);
        const std::vector<glm::vec3> expected = expected_display(resolved, w, h, settings, nullptr);
        const auto gpu = read_display(*device, frame);
        CHECK(count_mismatches(gpu, expected, w) == 0);

        // Where the history shows: the block's leading edge (x = 12) blends the previous, dimmer block, so the display differs from the frame
        // alone. Pixels inside the block are clamped to their current color box, so their history has no effect.
        if (step.frame_index == 4) {
            const std::size_t inside = static_cast<std::size_t>(14) * w + 12;
            const std::vector<glm::vec3> current_only = expected_display(hdr, w, h, settings, nullptr);
            CHECK(std::abs(static_cast<int>(gpu[inside][0]) - to_byte(current_only[inside].r)) > 5);
        }
        destroy_frame(*device, frame);
    }

    // A resize: a new size and new inputs. The history is discarded, so the frame is its current color alone.
    require_ok(post->resize(24, 20));
    const std::uint32_t rw = 24;
    const std::uint32_t rh = 20;
    const std::vector<glm::vec3> hdr(static_cast<std::size_t>(rw) * rh, glm::vec3(0.5f));
    const std::vector<glm::vec2> motion(hdr.size(), glm::vec2(0.0f));
    TestFrame frame = make_frame(*device, rw, rh, hdr, motion);
    require_ok(post->run(PostInputs{frame.color, frame.velocity, frame.readback, camera, false}));
    CHECK(post->last_reset() == HistoryReset::Resize);
    CHECK(count_mismatches(read_display(*device, frame), expected_display(hdr, rw, rh, settings, nullptr), rw) == 0);
    destroy_frame(*device, frame);

    require_ok(post->release());
    require_ok(device->check_debug_messages());
}

// The scene renderer with the post pass on: the golden scene is drawn in linear HDR, post-processed, and read back. Picking and depth come
// from the scene targets, unchanged by the post pass; the display shows the tone-mapped red and a black background. The renderer runs the
// TAA history, a camera cut, a teleport, and a resize.
void run_scene_case(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));

    SceneRendererDesc desc;
    desc.width = kSceneWidth;
    desc.height = kSceneHeight;
    desc.post = test_settings();
    const auto load = [backend](const std::string& name) { return load_shader(backend, name); };
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load("scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load("scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load("scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load("scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load("scene_shadow_pixel")};
    shaders.post = post_shaders(backend);
    auto renderer = take(SceneRenderer::create(*device, desc, shaders));
    REQUIRE(renderer->post_enabled());

    GoldenScene scene = build_golden_scene();
    for (const engine::render::MeshData& mesh : scene.meshes) {
        take(renderer->upload_mesh(mesh));
    }

    // `near` is set while the camera is at the golden view: the near box fills the center, so its pixels are checked. After a 100-unit move the
    // box is under a pixel wide and may fall between samples, so those frames check that the scene draws real entities instead.
    const auto render_frame = [&](const char* label, HistoryReset expected, std::uint32_t size, bool near) {
        INFO(label);
        const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, renderer->bounds(), scene.renderables);
        require_ok(renderer->render(snapshot));
        CHECK(renderer->last_history_reset() == expected);
        const SceneImage& image = renderer->image();
        CHECK(image.width == size);
        const std::uint32_t center = size / 2;
        if (near) {
            // The center is the near box: its pick and depth come from the scene targets, and its color is the tone-mapped red.
            const auto picked = pick_entity(image, snapshot, center, center);
            CHECK(picked.has_value());
            if (picked.has_value()) {
                CHECK(*picked == scene.entities.near_box);
            }
            const auto& red = image.color[image.index(center, center)];
            CHECK(red[0] > 150);
            CHECK(red[1] < 40);
            CHECK(red[2] < 40);
            CHECK(image.depth[image.index(center, center)] > 0.0f);
        } else {
            // Every pick names an entity of the snapshot, and some pixels are drawn.
            std::size_t drawn = 0;
            std::size_t named = 0;
            for (const std::uint32_t id : image.pick_ids) {
                if (id != 0) {
                    ++drawn;
                    named += snapshot.entity_of(id).has_value() ? 1 : 0;
                }
            }
            CHECK(drawn > 0);
            CHECK(named == drawn);
        }
        // The background's scene color is zero, and the tone map keeps zero at zero.
        const auto& corner = image.color[image.index(0, 0)];
        CHECK(corner[0] == 0);
        CHECK(corner[1] == 0);
        CHECK(corner[2] == 0);
    };
    render_frame("first frame", HistoryReset::FirstFrame, kSceneWidth, true);
    render_frame("second frame", HistoryReset::None, kSceneWidth, true);
    renderer->notify_camera_cut();
    render_frame("after a camera cut", HistoryReset::CameraCut, kSceneWidth, true);
    // A 100-unit move in one frame is a teleport. The golden view still looks down -Z at the near box, so the center is the same entity.
    scene.camera.position.z += 100.0;
    render_frame("after a teleport", HistoryReset::Teleport, kSceneWidth, false);
    // The camera returns to the golden view while the target is resized. A resize takes precedence over a teleport, so the reason is Resize.
    scene.camera.position.z -= 100.0;
    require_ok(renderer->resize(kSceneWidth / 2, kSceneHeight / 2));
    render_frame("after a resize", HistoryReset::Resize, kSceneWidth / 2, true);
    require_ok(renderer->set_post_settings(test_settings()));
    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    require_ok(device->check_debug_messages());
}

}  // namespace

TEST_CASE("DX12 post pass: NaN, Inf, and negative HDR samples are sanitized before bloom and tone mapping", "[gpu][dx12]") {
    run_non_finite_case(Backend::Dx12);
}

TEST_CASE("Vulkan post pass: NaN, Inf, and negative HDR samples are sanitized before bloom and tone mapping", "[gpu][vulkan]") {
    run_non_finite_case(Backend::Vulkan);
}

TEST_CASE("DX12 post pass: the reference overlay shows the same colors at two exposures", "[gpu][dx12]") {
    run_overlay_case(Backend::Dx12);
}

TEST_CASE("Vulkan post pass: the reference overlay shows the same colors at two exposures", "[gpu][vulkan]") {
    run_overlay_case(Backend::Vulkan);
}

TEST_CASE("DX12 TAA history follows a moving block, and a cut, a teleport, and a resize reset it", "[gpu][dx12]") {
    run_history_case(Backend::Dx12);
}

TEST_CASE("Vulkan TAA history follows a moving block, and a cut, a teleport, and a resize reset it", "[gpu][vulkan]") {
    run_history_case(Backend::Vulkan);
}

TEST_CASE("DX12 scene renderer with the post pass: picks and depth come from the scene, the display is tone-mapped", "[gpu][dx12]") {
    run_scene_case(Backend::Dx12);
}

TEST_CASE("Vulkan scene renderer with the post pass: picks and depth come from the scene, the display is tone-mapped", "[gpu][vulkan]") {
    run_scene_case(Backend::Vulkan);
}
