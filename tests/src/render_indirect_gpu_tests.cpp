// GPU indirect draws (US-037) on real GPUs. The same frame is drawn twice on one device: once indexed, the reference path, and once with
// GPU culling and one indirect count draw per group. The two images must agree pixel for pixel, and the GPU's visible count must match the
// CPU's drawn count from extraction. The cases cover culling disabled, a group past its capacity (overflow), an empty frame, and skinned
// groups. Each run has the debug layers on, so any validation error fails the check.

#include "render_scene_fixture.hpp"

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/reference_raster.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"

#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using engine::render::Camera;
using engine::render::DrawPath;
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::IndirectStats;
using engine::render::Light;
using engine::render::LightType;
using engine::render::MeshData;
using engine::render::RenderableRef;
using engine::render::rasterize_reference;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::SkinPose;
using engine::render::tests::build_golden_scene;
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

SceneShaders load_shaders(Backend backend) {
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load_shader(backend, "scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load_shader(backend, "scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load_shader(backend, "scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load_shader(backend, "scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load_shader(backend, "scene_shadow_pixel")};
    shaders.skinned_vertex = ShaderBinary{ShaderStage::Vertex, "VSSkinned", load_shader(backend, "scene_skinned_vertex")};
    shaders.skinned_shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSSkinnedShadow", load_shader(backend, "scene_skinned_shadow_vertex")};
    shaders.indirect_vertex = ShaderBinary{ShaderStage::Vertex, "VSIndirect", load_shader(backend, "scene_indirect_vertex")};
    shaders.skinned_indirect_vertex = ShaderBinary{ShaderStage::Vertex, "VSSkinnedIndirect", load_shader(backend, "scene_skinned_indirect_vertex")};
    shaders.cull = ShaderBinary{ShaderStage::Compute, "CSCull", load_shader(backend, "scene_cull")};
    return shaders;
}

// A renderer for the golden scene's target. `capacity` is the records per group (zero: max_items), and `culling` is the GPU culling switch.
std::unique_ptr<SceneRenderer> make_renderer(Device& device, Backend backend, DrawPath path, std::uint32_t capacity, bool culling,
                                             const std::vector<MeshData>& meshes) {
    SceneRendererDesc desc;
    desc.width = kSceneWidth;
    desc.height = kSceneHeight;
    desc.draw_path = path;
    desc.draw_group_capacity = capacity;
    desc.gpu_culling = culling;
    auto renderer = take(SceneRenderer::create(device, desc, load_shaders(backend)));
    for (const MeshData& mesh : meshes) {
        take(renderer->upload_mesh(mesh));
    }
    return renderer;
}

// The two images show the same thing: the same entity at every pixel, colors within one step, and depth within float error. Both paths run
// the same shader on the same records, so they should be identical; the tolerance only allows for the order of the draws.
void check_same_image(const SceneImage& indexed, const SceneImage& indirect) {
    REQUIRE(indexed.pick_ids.size() == indirect.pick_ids.size());
    std::size_t id_mismatches = 0;
    int worst_color = 0;
    double worst_depth = 0.0;
    for (std::size_t i = 0; i < indexed.pick_ids.size(); ++i) {
        if (indexed.pick_ids[i] != indirect.pick_ids[i]) {
            ++id_mismatches;
            continue;
        }
        for (std::size_t c = 0; c < 4; ++c) {
            worst_color = std::max(worst_color, std::abs(static_cast<int>(indexed.color[i][c]) - static_cast<int>(indirect.color[i][c])));
        }
        worst_depth = std::max(worst_depth, std::abs(static_cast<double>(indexed.depth[i]) - static_cast<double>(indirect.depth[i])));
    }
    INFO("pick ids that differ between the indexed and indirect frames: " << id_mismatches);
    CHECK(id_mismatches == 0);
    INFO("worst color difference: " << worst_color);
    CHECK(worst_color <= 1);
    INFO("worst depth difference: " << worst_depth);
    CHECK(worst_depth <= 1e-6);
}

// The indirect frame agrees with the CPU reference, as the direct scene tests do: pick ids except on edge pixels, and depth where both see
// a surface.
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

void run_golden_agreement(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const auto baseline = take(device->resource_stats());
    GoldenScene scene = build_golden_scene();
    auto indexed = make_renderer(*device, backend, DrawPath::Indexed, 0, true, scene.meshes);
    auto indirect = make_renderer(*device, backend, DrawPath::Indirect, 0, true, scene.meshes);

    const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, indexed->bounds(), scene.renderables);
    require_ok(indexed->render(snapshot));
    require_ok(indirect->render(snapshot));

    CHECK_FALSE(indexed->indirect_stats().active);
    const IndirectStats& stats = indirect->indirect_stats();
    CHECK(stats.active);
    CHECK(stats.groups == 2);         // (box, default material) and (half box, default material)
    CHECK(stats.indexed_groups == 0);
    CHECK(stats.candidates == 4);     // the four opaque objects the CPU kept
    // The GPU kept what the CPU kept: the visible count is the reference path's drawn count, and nothing overflowed.
    CHECK(stats.visible == snapshot.stats().drawn);
    CHECK(stats.drawn == stats.visible);
    CHECK(stats.overflow == 0);
    check_same_image(indexed->image(), indirect->image());
    check_against_reference(indirect->image(), snapshot, indirect->meshes());

    require_ok(device->check_debug_messages());
    require_ok(indexed->release());
    require_ok(indirect->release());
    const auto after = take(device->resource_stats());
    CHECK(after.buffers == baseline.buffers);
    CHECK(after.descriptor_sets == baseline.descriptor_sets);
    require_ok(device->check_debug_messages());
}

void run_disabled_culling(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    GoldenScene scene = build_golden_scene();
    auto indexed = make_renderer(*device, backend, DrawPath::Indexed, 0, true, scene.meshes);
    auto unculled = make_renderer(*device, backend, DrawPath::Indirect, 0, false, scene.meshes);

    const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, indexed->bounds(), scene.renderables);
    require_ok(indexed->render(snapshot));
    require_ok(unculled->render(snapshot));

    // With culling off the GPU keeps every candidate, so the visible count is the candidate count, and the frame is the indexed frame.
    const IndirectStats& stats = unculled->indirect_stats();
    CHECK(stats.active);
    CHECK(stats.candidates == 4);
    CHECK(stats.visible == stats.candidates);
    CHECK(stats.drawn == stats.candidates);
    check_same_image(indexed->image(), unculled->image());

    require_ok(device->check_debug_messages());
    require_ok(indexed->release());
    require_ok(unculled->release());
}

void run_overflow(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    GoldenScene scene = build_golden_scene();
    // Two records per group: the box group holds three visible objects, so one of them does not fit and is not drawn.
    auto capped = make_renderer(*device, backend, DrawPath::Indirect, 2, true, scene.meshes);

    const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, capped->bounds(), scene.renderables);
    require_ok(capped->render(snapshot));

    const IndirectStats& stats = capped->indirect_stats();
    CHECK(stats.active);
    CHECK(stats.candidates == 4);
    CHECK(stats.visible == 4);
    CHECK(stats.drawn == 3);
    CHECK(stats.overflow == 1);
    CHECK(stats.visible == stats.drawn + stats.overflow);

    // Only drawn entities appear, and the single-object group is always drawn. The center holds the near box, or the far box if the near
    // box is the one that did not fit.
    const SceneImage& image = capped->image();
    for (const std::uint32_t id : image.pick_ids) {
        CHECK(id <= 4);
    }
    CHECK(image.pick_ids[image.index(49, 32)] == 3);
    const std::uint32_t center = image.pick_ids[image.index(32, 32)];
    CHECK((center == 1 || center == 2));

    require_ok(device->check_debug_messages());
    require_ok(capped->release());
}

void run_empty_frame(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    GoldenScene scene = build_golden_scene();
    auto renderer = make_renderer(*device, backend, DrawPath::Indirect, 0, true, scene.meshes);

    // No renderables: no groups, so nothing is indirect and nothing is drawn.
    const FrameSnapshot empty = extract_frame(scene.graph, scene.camera, renderer->bounds(), {});
    require_ok(renderer->render(empty));
    const IndirectStats& none = renderer->indirect_stats();
    CHECK_FALSE(none.active);
    CHECK(none.groups == 0);
    CHECK(none.candidates == 0);
    CHECK(none.visible == 0);
    for (const std::uint32_t id : renderer->image().pick_ids) {
        CHECK(id == 0);
    }

    // The next frame with objects draws them indirectly, with no stale state from the empty frame.
    const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, renderer->bounds(), scene.renderables);
    require_ok(renderer->render(snapshot));
    CHECK(renderer->indirect_stats().active);
    CHECK(renderer->indirect_stats().visible == snapshot.stats().drawn);
    CHECK(renderer->image().pick_ids[renderer->image().index(32, 32)] == 1);

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
}

// A box whose top half is bound to joint 0 and whose bottom half blends joints 0 and 1 (as in render_skin_gpu_tests).
MeshData two_bone_box() {
    MeshData mesh = engine::render::make_box_mesh(glm::vec3(0.5f));
    mesh.joints.resize(mesh.positions.size());
    mesh.weights.resize(mesh.positions.size());
    for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
        if (mesh.positions[v].y >= 0.0f) {
            mesh.joints[v] = glm::uvec4(0, 0, 0, 0);
            mesh.weights[v] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        } else {
            mesh.joints[v] = glm::uvec4(0, 1, 0, 0);
            mesh.weights[v] = glm::vec4(0.5f, 0.5f, 0.0f, 0.0f);
        }
    }
    return mesh;
}

void run_skinned_agreement(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const MeshData box = engine::render::make_box_mesh(glm::vec3(0.5f));
    const MeshData skinned = two_bone_box();
    const std::vector<MeshData> meshes{box, skinned};
    auto indexed = make_renderer(*device, backend, DrawPath::Indexed, 0, true, meshes);
    auto indirect = make_renderer(*device, backend, DrawPath::Indirect, 0, true, meshes);

    Camera camera;
    camera.position = glm::dvec3(0.0, 0.0, 3.0);
    camera.aspect = 1.0;
    engine::scene::SceneGraph graph;
    engine::scene::Transform transform;
    transform.position = glm::dvec3(0.0, 0.0, -2.0);
    const engine::ecs::EntityHandle node = graph.create(transform).value();

    // A sun from above, so the skinned box also casts into the cascades (the shadow draws stay indexed).
    Light sun;
    sun.id = 1;
    sun.type = LightType::Directional;
    sun.direction = glm::normalize(glm::vec3(0.0f, -1.0f, -0.3f));
    sun.casts_shadow = true;
    const std::vector<Light> lights{sun};

    // Frame 1 is the bind pose. Frame 2 moves joint 1 up, so the bottom half has motion and the frame has motion vectors.
    SkinPose first;
    first.palette = {glm::mat4(1.0f), glm::mat4(1.0f)};
    SkinPose second;
    second.palette = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.25f, 0.0f))};
    second.previous = first.palette;

    RenderableRef ref;
    ref.node = node;
    ref.mesh = 1;
    for (SkinPose* pose : {&first, &second}) {
        ref.skin = pose;
        const FrameSnapshot snapshot = extract_frame(graph, camera, indexed->bounds(), {ref}, lights);
        REQUIRE(snapshot.items().size() == 1);
        require_ok(indexed->render(snapshot));
        require_ok(indirect->render(snapshot));

        const IndirectStats& stats = indirect->indirect_stats();
        CHECK(stats.active);
        CHECK(stats.groups == 1);
        CHECK(stats.candidates == 1);
        CHECK(stats.visible == 1);
        CHECK(stats.drawn == 1);
        check_same_image(indexed->image(), indirect->image());
        for (std::size_t i = 0; i < indexed->image().velocity.size(); ++i) {
            const glm::vec2 delta = indexed->image().velocity[i] - indirect->image().velocity[i];
            CHECK(std::max(std::abs(delta.x), std::abs(delta.y)) <= 1e-6f);
        }
    }

    require_ok(device->check_debug_messages());
    require_ok(indexed->release());
    require_ok(indirect->release());
}

}  // namespace

TEST_CASE("DX12 indirect draws match the indexed path on the golden scene, and its visible count matches the CPU", "[gpu][dx12]") {
    run_golden_agreement(Backend::Dx12);
}

TEST_CASE("DX12 indirect draws with GPU culling disabled draw every candidate", "[gpu][dx12]") {
    run_disabled_culling(Backend::Dx12);
}

TEST_CASE("DX12 indirect draws past a group's capacity drop the overflow and write nothing beyond it", "[gpu][dx12]") {
    run_overflow(Backend::Dx12);
}

TEST_CASE("DX12 an empty frame draws nothing indirectly, and the next frame is not affected", "[gpu][dx12]") {
    run_empty_frame(Backend::Dx12);
}

TEST_CASE("DX12 skinned indirect draws match the indexed path, including motion vectors", "[gpu][dx12]") {
    run_skinned_agreement(Backend::Dx12);
}

TEST_CASE("Vulkan indirect draws match the indexed path on the golden scene, and its visible count matches the CPU", "[gpu][vulkan]") {
    run_golden_agreement(Backend::Vulkan);
}

TEST_CASE("Vulkan indirect draws with GPU culling disabled draw every candidate", "[gpu][vulkan]") {
    run_disabled_culling(Backend::Vulkan);
}

TEST_CASE("Vulkan indirect draws past a group's capacity drop the overflow and write nothing beyond it", "[gpu][vulkan]") {
    run_overflow(Backend::Vulkan);
}

TEST_CASE("Vulkan an empty frame draws nothing indirectly, and the next frame is not affected", "[gpu][vulkan]") {
    run_empty_frame(Backend::Vulkan);
}

TEST_CASE("Vulkan skinned indirect draws match the indexed path, including motion vectors", "[gpu][vulkan]") {
    run_skinned_agreement(Backend::Vulkan);
}
