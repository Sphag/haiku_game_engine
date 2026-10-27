// Skinning and motion vectors on real GPUs (US-036). A two-bone box is skinned by the scene shader on DX12 and on Vulkan with the debug layers
// on. The first frame has no motion, and the second frame moves one of the bones, so the frame has motion vectors. Each frame's pick ids, depth,
// and motion vectors are checked against the CPU reference, which skins the same palettes. A sun that casts shadows also draws the skinned
// caster through the shadow pipeline. Any validation error fails the check.

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/reference_raster.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"

#include "engine/rhi/device.hpp"
#include "engine/scene/scene_graph.hpp"
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
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::Light;
using engine::render::LightType;
using engine::render::MeshData;
using engine::render::MotionHistory;
using engine::render::RenderableRef;
using engine::render::rasterize_reference;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::SkinPose;
using engine::rhi::Backend;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;

namespace {

using Catch::Matchers::WithinAbs;

constexpr std::uint32_t kSize = 64;

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

SceneShaders load_shaders(Backend backend) {
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load_shader(backend, "scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load_shader(backend, "scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load_shader(backend, "scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load_shader(backend, "scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load_shader(backend, "scene_shadow_pixel")};
    shaders.skinned_vertex = ShaderBinary{ShaderStage::Vertex, "VSSkinned", load_shader(backend, "scene_skinned_vertex")};
    shaders.skinned_shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSSkinnedShadow", load_shader(backend, "scene_skinned_shadow_vertex")};
    return shaders;
}

// A box whose top half is bound to joint 0 and whose bottom half is blended between joints 0 and 1.
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

// The two frames' palettes. Joint 1 rises 0.25 m between them, so the bottom half moves and the top half does not.
SkinPose first_pose() {
    SkinPose pose;
    pose.palette = {glm::mat4(1.0f), glm::mat4(1.0f)};
    return pose;
}

SkinPose second_pose(const SkinPose& previous) {
    SkinPose pose;
    pose.palette = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.25f, 0.0f))};
    pose.previous = previous.palette;
    return pose;
}

// The GPU image agrees with the reference: pick ids and depth everywhere both see a surface (a few boundary pixels may differ, as in the
// scene tests), and motion vectors to within float error.
void check_against_reference(const SceneImage& gpu, const SceneImage& reference, bool expect_motion) {
    REQUIRE(gpu.pick_ids.size() == reference.pick_ids.size());
    std::size_t id_mismatches = 0;
    double worst_depth = 0.0;
    double worst_velocity = 0.0;
    std::size_t moving = 0;
    for (std::size_t i = 0; i < gpu.pick_ids.size(); ++i) {
        if (gpu.pick_ids[i] != reference.pick_ids[i]) {
            ++id_mismatches;
            continue;
        }
        worst_depth = std::max(worst_depth, std::abs(static_cast<double>(gpu.depth[i]) - static_cast<double>(reference.depth[i])));
        const glm::vec2 delta = gpu.velocity[i] - reference.velocity[i];
        worst_velocity = std::max(worst_velocity, static_cast<double>(std::max(std::abs(delta.x), std::abs(delta.y))));
        moving += glm::length(reference.velocity[i]) > 1e-4f ? 1 : 0;
    }
    INFO("pick ids that differ from the reference: " << id_mismatches);
    CHECK(id_mismatches <= 4);
    CHECK(worst_depth <= 1e-4);
    INFO("worst motion vector difference: " << worst_velocity);
    CHECK(worst_velocity <= 2e-3);
    CHECK((moving > 0) == expect_motion);
}

void run_skinned_scene(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const auto baseline = take(device->resource_stats());

    SceneRendererDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    auto renderer = take(SceneRenderer::create(*device, desc, load_shaders(backend)));

    const MeshData box = engine::render::make_box_mesh(glm::vec3(0.5f));
    const MeshData skinned = two_bone_box();
    take(renderer->upload_mesh(box));
    take(renderer->upload_mesh(skinned));
    const std::vector<MeshData> meshes{box, skinned};

    Camera camera;
    camera.position = glm::dvec3(0.0, 0.0, 3.0);
    camera.aspect = 1.0;
    engine::scene::SceneGraph graph;
    engine::scene::Transform transform;
    transform.position = glm::dvec3(0.0, 0.0, -2.0);
    const engine::ecs::EntityHandle node = graph.create(transform).value();

    // A sun from above that casts shadows, so the skinned box also draws into the cascades.
    Light sun;
    sun.id = 1;
    sun.type = LightType::Directional;
    sun.direction = glm::normalize(glm::vec3(0.0f, -1.0f, -0.3f));
    sun.casts_shadow = true;
    const std::vector<Light> lights{sun};

    // Frame 1: the palette has not moved, so there is no motion anywhere.
    SkinPose pose_one = first_pose();
    RenderableRef skinned_ref;
    skinned_ref.node = node;
    skinned_ref.mesh = 1;
    skinned_ref.skin = &pose_one;
    const FrameSnapshot first = extract_frame(graph, camera, renderer->bounds(), {skinned_ref}, lights);
    REQUIRE(first.items().size() == 1);
    require_ok(renderer->render(first));
    // Zero motion up to float error: the current and previous positions run the same arithmetic, but the compiler may fuse it differently.
    const SceneImage& first_image = renderer->image();
    float largest_first_frame_motion = 0.0f;
    for (const glm::vec2& velocity : first_image.velocity) {
        largest_first_frame_motion = std::max(largest_first_frame_motion, std::max(std::abs(velocity.x), std::abs(velocity.y)));
    }
    INFO("largest first-frame motion vector component: " << largest_first_frame_motion);
    CHECK(largest_first_frame_motion <= 1e-5f);
    check_against_reference(first_image, rasterize_reference(first, meshes, kSize, kSize), false);

    // Frame 2: joint 1 has risen, and the previous frame's palette is kept for the motion vectors.
    SkinPose pose_two = second_pose(pose_one);
    skinned_ref.skin = &pose_two;
    const FrameSnapshot second = extract_frame(graph, camera, renderer->bounds(), {skinned_ref}, lights);
    require_ok(renderer->render(second));
    const MotionHistory previous{first.view_projection(), first.camera().position};
    check_against_reference(renderer->image(), rasterize_reference(second, meshes, kSize, kSize, previous), true);
    CHECK(renderer->lighting_stats().shadow_draws > 0);

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    const auto after = take(device->resource_stats());
    CHECK(after.buffers == baseline.buffers);
    CHECK(after.textures == baseline.textures);
    CHECK(after.descriptor_sets == baseline.descriptor_sets);
    require_ok(device->check_debug_messages());
}

}  // namespace

TEST_CASE("DX12 skins a two-bone mesh, moves it between frames, and writes motion vectors that match the reference", "[gpu][dx12]") {
    run_skinned_scene(Backend::Dx12);
}

TEST_CASE("Vulkan skins a two-bone mesh, moves it between frames, and writes motion vectors that match the reference", "[gpu][vulkan]") {
    run_skinned_scene(Backend::Vulkan);
}
