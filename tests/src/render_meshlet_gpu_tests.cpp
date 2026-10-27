// GPU mesh shader path (US-038) on real GPUs. The golden scene is drawn three ways on one device: through the mesh shaders on the cooked
// meshlets, through the conventional indirect path with the same cooked data (set_geometry_path switches without an upload), and with mesh
// shaders forcibly disabled. The meshlet frame must agree with the conventional frame pixel for pixel, and both must agree with the CPU
// reference within the scene tests' tolerance. The GPU's visible count must match the CPU reference's count of (item, meshlet) pairs, and a
// flipped plane must be culled by its normal cone. Every run has the debug layers on, so any validation error fails the check. A device without
// mesh shaders reports the cases as skipped, not passed.

#include "render_meshlet_fixture.hpp"
#include "render_scene_fixture.hpp"

#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/material.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/meshlet.hpp"
#include "engine/render/reference_raster.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"

#include "engine/rhi/device.hpp"
#include "engine/scene/scene_graph.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

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
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::GeometryPath;
using engine::render::make_box_mesh;
using engine::render::make_frustum;
using engine::render::make_plane_mesh;
using engine::render::MeshData;
using engine::render::MeshletStats;
using engine::render::meshlet_visible;
using engine::render::RenderableRef;
using engine::render::rasterize_reference;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::tests::build_golden_scene;
using engine::render::tests::cook_for_render;
using engine::render::tests::CookedMesh;
using engine::render::tests::GoldenScene;
using engine::render::tests::kSceneHeight;
using engine::render::tests::kSceneWidth;
using engine::rhi::Backend;
using engine::rhi::CapabilityReport;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::Feature;
using engine::rhi::FeatureState;
using engine::rhi::RhiErrorCode;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;

namespace {

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
    shaders.meshlet_task = ShaderBinary{ShaderStage::Amplification, "ASMeshlet", load_shader(backend, "scene_meshlet_task")};
    shaders.meshlet_mesh = ShaderBinary{ShaderStage::Mesh, "MSMeshlet", load_shader(backend, "scene_meshlet_mesh")};
    return shaders;
}

// Two images show the same thing when every pixel has the same entity, colors are within one step, and depth is within float error. The
// meshlet and conventional paths shade the same records with the same shader; only the order of the triangles differs.
void check_same_image(const SceneImage& expected, const SceneImage& actual) {
    REQUIRE(expected.pick_ids.size() == actual.pick_ids.size());
    std::size_t id_mismatches = 0;
    int worst_color = 0;
    double worst_depth = 0.0;
    for (std::size_t i = 0; i < expected.pick_ids.size(); ++i) {
        if (expected.pick_ids[i] != actual.pick_ids[i]) {
            ++id_mismatches;
            continue;
        }
        for (std::size_t c = 0; c < 4; ++c) {
            worst_color = std::max(worst_color, std::abs(static_cast<int>(expected.color[i][c]) - static_cast<int>(actual.color[i][c])));
        }
        worst_depth = std::max(worst_depth, std::abs(static_cast<double>(expected.depth[i]) - static_cast<double>(actual.depth[i])));
    }
    INFO("pick ids that differ between the two frames: " << id_mismatches);
    CHECK(id_mismatches == 0);
    INFO("worst color difference: " << worst_color);
    CHECK(worst_color <= 1);
    INFO("worst depth difference: " << worst_depth);
    CHECK(worst_depth <= 1e-6);
}

// The image agrees with the CPU reference: pick ids except on edge pixels, and depth where both see a surface (the scene tests' bounds).
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

// The (item, meshlet) pairs the CPU reference keeps: every opaque item against each meshlet of its mesh, under the same frustum test, and the
// cone test unless the item's material is two-sided (its back faces draw).
std::uint32_t reference_visible(const SceneRenderer& renderer, const FrameSnapshot& snapshot, const std::vector<CookedMesh>& meshes) {
    const engine::render::Frustum frustum = make_frustum(snapshot.view_projection());
    std::uint32_t visible = 0;
    for (const engine::render::SnapshotItem& item : snapshot.items()) {
        if (item.pick_id == 0) {
            continue;  // shadow-only
        }
        const bool cone_test = !renderer.material_params(item.material).two_sided;
        for (const engine::render::MeshletCluster& meshlet : meshes[item.mesh].meshlets.meshlets) {
            visible += meshlet_visible(meshlet, item.model, frustum, cone_test) ? 1u : 0u;
        }
    }
    return visible;
}

// A plane whose winding and normals face -Z, so it faces away from the camera.
MeshData flipped_plane(float half_size) {
    MeshData flipped = make_plane_mesh(half_size);
    for (glm::vec3& normal : flipped.normals) {
        normal = -normal;
    }
    for (std::size_t t = 0; t < flipped.indices.size(); t += 3) {
        std::swap(flipped.indices[t + 1], flipped.indices[t + 2]);
    }
    return flipped;
}

std::uint32_t total_candidates(const FrameSnapshot& snapshot, const std::vector<CookedMesh>& meshes) {
    std::uint32_t candidates = 0;
    for (const engine::render::SnapshotItem& item : snapshot.items()) {
        if (item.pick_id != 0) {
            candidates += static_cast<std::uint32_t>(meshes[item.mesh].meshlets.meshlets.size());
        }
    }
    return candidates;
}

std::unique_ptr<SceneRenderer> make_renderer(Device& device, Backend backend, const std::vector<CookedMesh>& meshes) {
    SceneRendererDesc desc;
    desc.width = kSceneWidth;
    desc.height = kSceneHeight;
    auto renderer = take(SceneRenderer::create(device, desc, load_shaders(backend)));
    for (const CookedMesh& mesh : meshes) {
        const std::uint32_t index = take(renderer->upload_mesh(mesh.mesh));
        require_ok(renderer->attach_meshlets(index, mesh.meshlets));
    }
    return renderer;
}

std::vector<CookedMesh> cook_all(const std::vector<MeshData>& meshes) {
    std::vector<CookedMesh> cooked;
    for (const MeshData& mesh : meshes) {
        cooked.push_back(cook_for_render(mesh));
    }
    return cooked;
}

// The mesh shader path agrees with the conventional path on the same cooked data, both agree with the CPU reference, and the GPU's visible
// count is the CPU's count. A second renderer shows that a full pool refuses the mesh instead of drawing it wrongly.
// Returns false, without drawing anything, when the adapter has no mesh shaders: the test case then reports the case as not run.
bool run_golden_agreement(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    if (!device->capabilities().enabled(Feature::MeshShaders)) {
        return false;
    }
    const auto baseline = take(device->resource_stats());
    GoldenScene scene = build_golden_scene();
    const std::vector<CookedMesh> cooked = cook_all(scene.meshes);
    auto renderer = make_renderer(*device, backend, cooked);
    REQUIRE(renderer->geometry_path() == GeometryPath::Meshlets);

    const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, renderer->bounds(), scene.renderables);
    require_ok(renderer->render(snapshot));
    const MeshletStats& stats = renderer->meshlet_stats();
    CHECK(stats.active);
    CHECK(stats.groups == 2);  // (box, default material) and (half box, default material)
    CHECK(stats.candidates == total_candidates(snapshot, cooked));
    // The GPU's culling kept the pairs the CPU's test keeps, and the counts are the same frame's.
    CHECK(stats.visible == reference_visible(*renderer, snapshot, cooked));
    CHECK(stats.visible <= stats.candidates);
    check_against_reference(renderer->image(), snapshot, renderer->meshes());
    const SceneImage meshlet_image = renderer->image();

    // The same renderer, the same cooked data, the conventional path: no upload, no new data.
    renderer->set_geometry_path(GeometryPath::Conventional);
    require_ok(renderer->render(snapshot));
    CHECK_FALSE(renderer->meshlet_stats().active);
    CHECK(renderer->indirect_stats().active);
    check_same_image(renderer->image(), meshlet_image);
    check_against_reference(renderer->image(), snapshot, renderer->meshes());
    require_ok(device->check_debug_messages());

    // A pool with room for one meshlet takes the first mesh and refuses the second, and nothing is drawn from a refused mesh.
    SceneRendererDesc small;
    small.width = kSceneWidth;
    small.height = kSceneHeight;
    small.meshlets.meshlets = 1;
    auto tight = take(SceneRenderer::create(*device, small, load_shaders(backend)));
    const std::uint32_t first = take(tight->upload_mesh(cooked[0].mesh));
    require_ok(tight->attach_meshlets(first, cooked[0].meshlets));
    const std::uint32_t second = take(tight->upload_mesh(cooked[1].mesh));
    const RhiStatus refused = tight->attach_meshlets(second, cooked[1].meshlets);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == RhiErrorCode::OutOfRange);
    CHECK_FALSE(tight->has_meshlets(second));

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    require_ok(tight->release());
    const auto after = take(device->resource_stats());
    CHECK(after.buffers == baseline.buffers);
    CHECK(after.descriptor_sets == baseline.descriptor_sets);
    require_ok(device->check_debug_messages());
    return true;
}

// With mesh shaders forcibly disabled, the device reports them as disabled, the renderer draws conventionally, and the attach fails with
// Unsupported. The image still matches the CPU reference.
bool run_forced_fallback(Backend backend) {
    DeviceDesc desc = debug_device(backend);
    desc.disabled_features = Feature::MeshShaders;
    auto device = take(engine::rhi::create_device(desc));
    const CapabilityReport& report = device->capabilities();
    if (report.state(Feature::MeshShaders) == FeatureState::Unsupported) {
        return false;  // the adapter has no mesh shaders, so there is nothing to disable
    }
    CHECK(report.state(Feature::MeshShaders) == FeatureState::DisabledForTest);
    CHECK_FALSE(report.enabled(Feature::MeshShaders));

    GoldenScene scene = build_golden_scene();
    const std::vector<CookedMesh> cooked = cook_all(scene.meshes);
    SceneRendererDesc renderer_desc;
    renderer_desc.width = kSceneWidth;
    renderer_desc.height = kSceneHeight;
    auto renderer = take(SceneRenderer::create(*device, renderer_desc, load_shaders(backend)));
    for (const CookedMesh& mesh : cooked) {
        const std::uint32_t index = take(renderer->upload_mesh(mesh.mesh));
        const RhiStatus attached = renderer->attach_meshlets(index, mesh.meshlets);
        REQUIRE_FALSE(attached.ok());
        CHECK(attached.error().code == RhiErrorCode::Unsupported);
    }
    CHECK(renderer->geometry_path() == GeometryPath::Conventional);

    const FrameSnapshot snapshot = extract_frame(scene.graph, scene.camera, renderer->bounds(), scene.renderables);
    require_ok(renderer->render(snapshot));
    CHECK_FALSE(renderer->meshlet_stats().active);
    CHECK(renderer->indirect_stats().active);
    check_against_reference(renderer->image(), snapshot, renderer->meshes());
    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    return true;
}

// A flipped plane faces away from the camera. Its one meshlet is culled by the normal cone on the GPU, as on the CPU, and its pixels are
// back-face culled anyway. The front plane and the box stay, so the visible count is below the candidate count.
bool run_cone_culling(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    if (!device->capabilities().enabled(Feature::MeshShaders)) {
        return false;
    }
    // Four meshes: a box, a front-facing plane, a small flipped plane (one-sided), and a large flipped plane. The small one sits behind the box,
    // so its pixels are hidden. The large one is two-sided, so its back faces draw and its cone must not cull it.
    const std::vector<MeshData> sources{make_box_mesh(glm::vec3(0.5f)), make_plane_mesh(1.0f), flipped_plane(0.25f), flipped_plane(1.0f)};
    const std::vector<CookedMesh> cooked = cook_all(sources);
    auto renderer = make_renderer(*device, backend, cooked);
    engine::render::MaterialParams two_sided_params;
    two_sided_params.two_sided = true;
    const std::uint32_t two_sided = take(renderer->create_material(two_sided_params));

    engine::scene::SceneGraph graph;
    Camera camera;
    camera.position = glm::dvec3(0.0, 0.0, 10.0);
    camera.fov_y_radians = 1.0471975511965976;
    camera.aspect = 1.0;
    camera.near_plane = 0.1;
    camera.far_plane = 1000.0;
    const auto place = [&graph](glm::dvec3 position) {
        engine::scene::Transform transform;
        transform.position = position;
        return graph.create(transform).value();
    };
    std::vector<RenderableRef> renderables;
    const auto add = [&renderables](engine::ecs::EntityHandle node, std::uint32_t mesh, std::uint32_t material) {
        RenderableRef renderable;
        renderable.node = node;
        renderable.mesh = mesh;
        renderable.material = material;
        renderable.color = glm::vec4(1.0f);
        renderables.push_back(renderable);
    };
    add(place(glm::dvec3(-2.0, 0.0, 0.0)), 0, 0);             // the box, in front of the camera
    add(place(glm::dvec3(2.0, 0.0, -6.0)), 1, 0);             // the plane that faces the camera
    add(place(glm::dvec3(-2.0, 0.0, -6.0)), 2, 0);            // the small flipped plane, one-sided: its cone faces away, and it hides behind the box
    add(place(glm::dvec3(-6.0, 0.0, -3.0)), 3, two_sided);    // the large flipped plane, two-sided: its back faces draw

    const FrameSnapshot snapshot = extract_frame(graph, camera, renderer->bounds(), renderables);
    require_ok(renderer->render(snapshot));
    const MeshletStats& stats = renderer->meshlet_stats();
    CHECK(stats.active);
    CHECK(stats.candidates == 4);
    CHECK(stats.visible == reference_visible(*renderer, snapshot, cooked));
    CHECK(stats.visible == 3);  // the box, the front plane, and the two-sided plane; the small one-sided flipped plane is culled
    check_against_reference(renderer->image(), snapshot, renderer->meshes());
    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    return true;
}

}  // namespace

TEST_CASE("DX12 mesh shaders match the conventional path on the golden scene and the CPU reference, and the visible count matches the CPU",
          "[gpu][dx12]") {
    if (!run_golden_agreement(Backend::Dx12)) {
        SKIP("the adapter has no mesh shaders (MeshShaders is unsupported): not run");
    }
}

TEST_CASE("DX12 mesh shaders forcibly disabled fall back to the conventional path and still match the CPU reference", "[gpu][dx12]") {
    if (!run_forced_fallback(Backend::Dx12)) {
        SKIP("the adapter has no mesh shaders (MeshShaders is unsupported): not run");
    }
}

TEST_CASE("DX12 mesh shaders cull a meshlet whose normal cone faces away, as the CPU does", "[gpu][dx12]") {
    if (!run_cone_culling(Backend::Dx12)) {
        SKIP("the adapter has no mesh shaders (MeshShaders is unsupported): not run");
    }
}

TEST_CASE("Vulkan mesh shaders match the conventional path on the golden scene and the CPU reference, and the visible count matches the CPU",
          "[gpu][vulkan]") {
    if (!run_golden_agreement(Backend::Vulkan)) {
        SKIP("the adapter has no mesh shaders (MeshShaders is unsupported): not run");
    }
}

TEST_CASE("Vulkan mesh shaders forcibly disabled fall back to the conventional path and still match the CPU reference", "[gpu][vulkan]") {
    if (!run_forced_fallback(Backend::Vulkan)) {
        SKIP("the adapter has no mesh shaders (MeshShaders is unsupported): not run");
    }
}

TEST_CASE("Vulkan mesh shaders cull a meshlet whose normal cone faces away, as the CPU does", "[gpu][vulkan]") {
    if (!run_cone_culling(Backend::Vulkan)) {
        SKIP("the adapter has no mesh shaders (MeshShaders is unsupported): not run");
    }
}
