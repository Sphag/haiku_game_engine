// Scene rendering on real GPUs (US-033). The golden scene runs on DX12 and on Vulkan with the debug layers on. Each run checks the
// golden pixels (color, entity ID, and depth), compares the whole frame with the CPU reference rasterizer, and repeats after the near box
// is destroyed. Any validation error fails the check. Vulkan takes the shader outputs for the same HLSL source, and the view-projection
// is flipped for Vulkan's NDC (docs/runtime-conventions.md) inside the renderer.

#include "render_material_fixture.hpp"
#include "render_scene_fixture.hpp"
#include "engine/render/pbr.hpp"

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/reference_raster.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"

#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::MeshData;
using engine::render::pick_entity;
using engine::render::rasterize_reference;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::Environment;
using engine::render::MaterialParams;
using engine::render::PixelReference;
using engine::render::tests::expected_pixel;
using engine::render::tests::quad_input;
using engine::render::tests::QuadSurface;
using engine::render::tests::build_golden_scene;
using engine::render::tests::golden_depth;
using engine::render::tests::GoldenScene;
using engine::render::tests::kSceneHeight;
using engine::render::tests::kSceneWidth;
using engine::rhi::Backend;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::scene::ChildPolicy;

namespace {

using Catch::Matchers::WithinAbs;

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

// DX12 loads the DXIL outputs and Vulkan loads the SPIR-V outputs of the same HLSL entry points (see tests/CMakeLists.txt).
std::vector<std::uint8_t> load_shader(Backend backend, const std::string& name) {
    const std::string path = std::string(ENGINE_TEST_SHADER_DIR) + "/" + name + (backend == Backend::Dx12 ? ".dxil" : ".spv");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FAIL("cannot open shader " << path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Golden pixels for the frame: the near box, the side box, and the background, then after the near box is destroyed.
// The color the CPU reference predicts for a flat surface in the plane through `center`, facing the camera, with the default material.
void check_color(const SceneImage& image, std::uint32_t x, std::uint32_t y, const FrameSnapshot& snapshot, const Environment& environment,
                 const glm::vec3& center, const glm::vec4& tint) {
    QuadSurface quad;
    quad.center = center;
    quad.half = 1.0f;
    const auto input = quad_input(snapshot.view_projection(), quad, x, y, kSceneWidth, kSceneHeight, tint, true);
    const PixelReference expected = expected_pixel(environment, input, MaterialParams{}, {});
    const std::size_t i = image.index(x, y);
    for (std::size_t c = 0; c < 3; ++c) {
        INFO("channel " << c << " at " << x << "," << y);
        CHECK(std::abs(static_cast<int>(image.color[i][c]) - static_cast<int>(expected.rgba[c])) <= 2);
    }
}

// Golden pixels for the frame: the near box, the side box, and the background, then after the near box is destroyed. Object colors come from
// the CPU reference, so they include the default material's lighting.
void check_golden_pixels(const SceneImage& image, const FrameSnapshot& snapshot, const Environment& environment, GoldenScene& scene, bool near_box_alive) {
    const auto pick_at = [&](std::uint32_t x, std::uint32_t y) { return pick_entity(image, snapshot, x, y); };
    const std::size_t center = image.index(32, 32);
    const std::size_t side = image.index(49, 32);
    const std::size_t corner = image.index(5, 5);

    if (near_box_alive) {
        CHECK(image.pick_ids[center] == 1);
        check_color(image, 32, 32, snapshot, environment, glm::vec3(0.0f, 0.0f, -9.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        CHECK_THAT(static_cast<double>(image.depth[center]), WithinAbs(golden_depth(9.0), 1e-4));
        CHECK(pick_at(32, 32) == scene.entities.near_box);
        CHECK(image.pick_ids[side] == 3);
        CHECK(pick_at(49, 32) == scene.entities.side_box);
        CHECK_THAT(static_cast<double>(image.depth[side]), WithinAbs(golden_depth(9.5), 1e-4));
        check_color(image, 49, 32, snapshot, environment, glm::vec3(3.0f, 0.0f, -9.5f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    } else {
        CHECK(image.pick_ids[center] == 1);
        check_color(image, 32, 32, snapshot, environment, glm::vec3(0.0f, 0.0f, -13.0f), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
        CHECK_THAT(static_cast<double>(image.depth[center]), WithinAbs(golden_depth(13.0), 1e-4));
        CHECK(pick_at(32, 32) == scene.entities.far_box);
        CHECK(image.pick_ids[side] == 2);
        CHECK(pick_at(49, 32) == scene.entities.side_box);
        CHECK_THAT(static_cast<double>(image.depth[side]), WithinAbs(golden_depth(9.5), 1e-4));
    }
    CHECK(image.pick_ids[corner] == 0);
    CHECK(image.color[corner] == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    CHECK_THAT(static_cast<double>(image.depth[corner]), WithinAbs(0.0, 1e-6));
    CHECK_FALSE(pick_at(5, 5).has_value());
}

// Counts pixels whose entity ID differs from the CPU reference, and reports the largest depth difference over the pixels that agree.
// The rasterizers may disagree on a pixel whose center lies exactly on a triangle edge, so the check allows a few such pixels.
void check_against_reference(const SceneImage& gpu, const FrameSnapshot& snapshot, const std::vector<MeshData>& meshes) {
    const SceneImage reference = rasterize_reference(snapshot, meshes, kSceneWidth, kSceneHeight);
    REQUIRE(gpu.pick_ids.size() == reference.pick_ids.size());
    std::size_t id_mismatches = 0;
    double worst_depth = 0.0;
    for (std::size_t i = 0; i < gpu.pick_ids.size(); ++i) {
        if (gpu.pick_ids[i] != reference.pick_ids[i]) {
            ++id_mismatches;
            continue;
        }
        worst_depth = std::max(worst_depth, std::abs(static_cast<double>(gpu.depth[i]) - static_cast<double>(reference.depth[i])));
    }
    INFO("pick ids that differ from the reference: " << id_mismatches);
    CHECK(id_mismatches <= 4);
    CHECK(worst_depth <= 1e-4);
}

void run_golden_scene(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const auto baseline = take(device->resource_stats());

    SceneRendererDesc desc;
    desc.width = kSceneWidth;
    desc.height = kSceneHeight;
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load_shader(backend, "scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load_shader(backend, "scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load_shader(backend, "scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load_shader(backend, "scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load_shader(backend, "scene_shadow_pixel")};
    // The indirect entries (US-037), so the default path draws the golden scene with GPU culling and indirect count draws.
    shaders.indirect_vertex = ShaderBinary{ShaderStage::Vertex, "VSIndirect", load_shader(backend, "scene_indirect_vertex")};
    shaders.cull = ShaderBinary{ShaderStage::Compute, "CSCull", load_shader(backend, "scene_cull")};
    auto renderer = take(SceneRenderer::create(*device, desc, shaders));

    GoldenScene scene = build_golden_scene();
    for (const MeshData& mesh : scene.meshes) {
        take(renderer->upload_mesh(mesh));
    }

    // Before: the near box covers the center.
    {
        const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, renderer->bounds(), scene.renderables);
        CHECK(snapshot.stats().drawn == 4);
        CHECK(snapshot.stats().culled == 2);
        CHECK(snapshot.stats().stale == 1);
        require_ok(renderer->render(snapshot));
        CHECK(renderer->indirect_stats().active);
        CHECK(renderer->indirect_stats().visible == snapshot.stats().drawn);
        check_golden_pixels(renderer->image(), snapshot, renderer->environment(), scene, true);
        check_against_reference(renderer->image(), snapshot, renderer->meshes());
    }

    // After the near box is destroyed: the far box is in front, and no frame may still hold the destroyed entity.
    static_cast<void>(scene.graph.destroy(scene.entities.near_box, ChildPolicy::DestroySubtree));
    {
        const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, renderer->bounds(), scene.renderables);
        CHECK(snapshot.stats().drawn == 3);
        require_ok(renderer->render(snapshot));
        check_golden_pixels(renderer->image(), snapshot, renderer->environment(), scene, false);
        check_against_reference(renderer->image(), snapshot, renderer->meshes());
        for (std::uint32_t id = 1; id <= snapshot.items().size(); ++id) {
            CHECK(snapshot.entity_of(id) != scene.entities.near_box);
        }
    }

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    const auto after = take(device->resource_stats());
    CHECK(after.buffers == baseline.buffers);
    CHECK(after.textures == baseline.textures);
    CHECK(after.descriptor_sets == baseline.descriptor_sets);
    require_ok(device->check_debug_messages());
}

}  // namespace

TEST_CASE("DX12 draws the golden scene: transforms, depth, and picking, including after an entity is deleted", "[gpu][dx12]") {
    run_golden_scene(Backend::Dx12);
}

TEST_CASE("Vulkan draws the golden scene: transforms, depth, and picking, including after an entity is deleted", "[gpu][vulkan]") {
    run_golden_scene(Backend::Vulkan);
}
