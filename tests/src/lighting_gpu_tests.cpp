// Lights and shadows on real GPUs (US-035). Each case runs the scene shader with the debug views the lighting header selects, and reads the
// targets back. The debug views write a value per pixel, so the checks read the shader's cluster lists, cascade choice, and shadow visibility
// directly, not through shading. Any validation error fails the check (debug layers are on).
//
// The scene: a ground box whose top is at y = 0, seen from above and behind; an occluder box 1.5 m above it; a sun travelling down and
// toward -Z; and a spot light and a point light placed to cover the cases. The debug views are encoded through sRGB, so the byte values are
// compared with thresholds rather than exact values.

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/pbr.hpp"
#include "engine/render/reference_raster.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"

#include "engine/rhi/device.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

using engine::render::Camera;
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::Light;
using engine::render::LightingSettings;
using engine::render::LightingStats;
using engine::render::LightingView;
using engine::render::LightType;
using engine::render::MeshData;
using engine::render::RenderableRef;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::rhi::Backend;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;

namespace {

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

SceneShaders shaders_for(Backend backend) {
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load_shader(backend, "scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load_shader(backend, "scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load_shader(backend, "scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load_shader(backend, "scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load_shader(backend, "scene_shadow_pixel")};
    return shaders;
}

// Small budgets that keep every atlas and cluster list tiny: 16-pixel tiles on a 64 x 64 target, 8 slices, 4 lights per cluster, and
// 256-texel atlases with 256-texel tiles.
SceneRendererDesc small_desc() {
    SceneRendererDesc desc;
    desc.width = kSize;
    desc.height = kSize;
    desc.max_items = 64;
    desc.lighting.max_lights = 16;
    desc.lighting.tile_px = 16;
    desc.lighting.slices = 8;
    desc.lighting.max_lights_per_cluster = 4;
    desc.cascades.count = 4;
    desc.cascades.shadow_distance = 60.0f;
    desc.cascades.caster_margin = 30.0f;
    desc.shadows.cascade_atlas_size = 512;
    desc.shadows.cascade_size = 256;
    desc.shadows.local_atlas_size = 512;
    desc.shadows.local_size = 256;
    desc.max_shadow_maps = 16;
    return desc;
}

// Cascade settings of the test renderer. Extraction culls with the same settings the renderer plans with.
const engine::render::CascadeSettings kCascades = small_desc().cascades;

// The scene the cases share: a ground box (mesh 0) and an occluder box (mesh 1), both as renderables, with the camera above and behind.
// The occluder floats 1 m above the ground, so its shadow lands in view on the ground behind it.
struct Stage {
    engine::scene::SceneGraph graph;
    std::vector<MeshData> meshes;
    std::vector<engine::render::MeshBounds> bounds;
    std::vector<RenderableRef> renderables;
    Camera camera;
    engine::ecs::EntityHandle ground;
    engine::ecs::EntityHandle occluder;
};

Stage make_stage(bool with_occluder, glm::dvec3 occluder_at = glm::dvec3(0.0, 2.5, -6.0)) {
    Stage stage;
    stage.camera.position = glm::dvec3(0.0, 3.0, 12.0);
    stage.camera.orientation = glm::angleAxis(-0.25, glm::dvec3(1.0, 0.0, 0.0));
    stage.camera.fov_y_radians = 1.0471975511965976;
    stage.camera.aspect = 1.0;
    stage.camera.near_plane = 0.1;
    stage.camera.far_plane = 200.0;
    stage.meshes = {engine::render::make_box_mesh(glm::vec3(40.0f, 0.5f, 40.0f)), engine::render::make_box_mesh(glm::vec3(1.5f))};
    for (const MeshData& mesh : stage.meshes) {
        stage.bounds.push_back(engine::render::compute_bounds(mesh));
    }

    const auto place = [&stage](glm::dvec3 position) {
        engine::scene::Transform transform;
        transform.position = position;
        return stage.graph.create(transform).value();
    };
    stage.ground = place(glm::dvec3(0.0, -0.5, -20.0));
    RenderableRef ground;
    ground.node = stage.ground;
    ground.mesh = 0;
    stage.renderables.push_back(ground);
    if (with_occluder) {
        stage.occluder = place(occluder_at);
        RenderableRef box;
        box.node = stage.occluder;
        box.mesh = 1;
        stage.renderables.push_back(box);
    }
    return stage;
}

Light sun(bool casts, float depth_bias = 0.0005f, float light_bias = 1.0f) {
    Light light;
    light.id = 1;
    light.type = LightType::Directional;
    light.direction = glm::normalize(glm::vec3(0.4f, -1.0f, -0.2f));
    light.color = glm::vec3(1.0f);
    light.intensity = 1.0f;
    light.casts_shadow = casts;
    light.depth_bias = depth_bias;
    light.light_bias = light_bias;
    light.pcf_radius = 1;
    return light;
}

Light point_at(std::uint32_t id, glm::vec3 position, float range) {
    Light light;
    light.id = id;
    light.type = LightType::Point;
    light.position = glm::dvec3(position);
    light.color = glm::vec3(1.0f);
    light.intensity = 4.0f;
    light.range = range;
    return light;
}

Light spot_at(std::uint32_t id, glm::vec3 position, glm::vec3 direction, bool casts) {
    Light light;
    light.id = id;
    light.type = LightType::Spot;
    light.position = glm::dvec3(position);
    light.direction = glm::normalize(direction);
    light.color = glm::vec3(1.0f);
    light.intensity = 6.0f;
    light.range = 30.0f;
    light.inner_angle = 0.4f;
    light.outer_angle = 0.7f;
    light.casts_shadow = casts;
    light.depth_bias = 0.0005f;
    light.light_bias = 1.0f;
    light.pcf_radius = 1;
    return light;
}

// Renders the stage with the lights, under one debug view, and returns the image. The renderer keeps running afterward, so the caller
// reads its stats.
SceneImage render_view(SceneRenderer& renderer, Stage& stage, const std::vector<Light>& lights, LightingView view) {
    renderer.set_lighting_view(view);
    const FrameSnapshot snapshot = extract_frame(stage.graph, stage.camera, stage.bounds, stage.renderables, lights, kCascades);
    require_ok(renderer.render(snapshot));
    return renderer.image();
}

// Pixels of the ground (pick id 1, the first renderable), as their 8-bit debug values.
std::vector<std::uint8_t> ground_values(const SceneImage& image) {
    std::vector<std::uint8_t> values;
    for (std::size_t i = 0; i < image.pick_ids.size(); ++i) {
        if (image.pick_ids[i] == 1) {
            values.push_back(image.color[i][0]);
        }
    }
    return values;
}

std::size_t count_below(const std::vector<std::uint8_t>& values, std::uint8_t threshold) {
    std::size_t count = 0;
    for (const std::uint8_t value : values) {
        count += value < threshold ? 1 : 0;
    }
    return count;
}

std::size_t count_above(const std::vector<std::uint8_t>& values, std::uint8_t threshold) {
    std::size_t count = 0;
    for (const std::uint8_t value : values) {
        count += value > threshold ? 1 : 0;
    }
    return count;
}

std::unique_ptr<SceneRenderer> make_renderer(Device& device, const SceneRendererDesc& desc, Backend backend) {
    return take(SceneRenderer::create(device, desc, shaders_for(backend)));
}

// Uploads a stage's meshes, in the order its renderables name them.
void upload_stage(SceneRenderer& renderer, const Stage& stage) {
    for (const MeshData& mesh : stage.meshes) {
        take(renderer.upload_mesh(mesh));
    }
}

}  // namespace

// ----- Clusters -----

void run_cluster_views(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto renderer = make_renderer(*device, small_desc(), backend);
    Stage stage = make_stage(false);
    upload_stage(*renderer, stage);

    // A point light straight ahead of the camera, 9.7 m away, with a 2.5 m reach: the center pixel's cluster holds it, and the top-left
    // corner, outside its footprint, holds nothing.
    const glm::vec3 ahead(0.0f, 0.5f, 2.3f);
    const std::vector<Light> lights{point_at(1, ahead, 2.5f)};
    const SceneImage counts = render_view(*renderer, stage, lights, LightingView::ClusterCount);
    const std::size_t center = counts.index(32, 32);
    CHECK(counts.pick_ids[center] == 1);
    CHECK(counts.color[center][0] > 0);
    CHECK(counts.color[counts.index(0, 0)][0] == 0);
    CHECK(renderer->lighting_stats().lights == 1);
    CHECK(renderer->lighting_stats().overflow_clusters == 0);

    // Six lights cover the same cluster; four fit. The overflow view marks that cluster red and keeps the others black.
    std::vector<Light> crowded;
    for (std::uint32_t i = 0; i < 6; ++i) {
        crowded.push_back(point_at(10 + i, ahead, 2.5f));
    }
    const SceneImage overflow = render_view(*renderer, stage, crowded, LightingView::ClusterOverflow);
    CHECK(overflow.color[overflow.index(32, 32)][0] == 255);
    CHECK(overflow.color[overflow.index(0, 0)][0] == 0);
    CHECK(renderer->lighting_stats().overflow_clusters >= 1);
    CHECK(renderer->lighting_stats().dropped_entries >= 2);

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
}

TEST_CASE("DX12 clusters: a point light reaches its cluster, and an overflowing cluster is flagged", "[gpu][dx12]") {
    run_cluster_views(Backend::Dx12);
}

TEST_CASE("Vulkan clusters: a point light reaches its cluster, and an overflowing cluster is flagged", "[gpu][vulkan]") {
    run_cluster_views(Backend::Vulkan);
}

// ----- Directional shadows -----

void run_directional_shadow(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto renderer = make_renderer(*device, small_desc(), backend);
    Stage stage = make_stage(true);
    upload_stage(*renderer, stage);

    // With the sun shadowing, the occluder darkens part of the ground and leaves the rest lit.
    const std::vector<Light> lit_sun{sun(true)};
    const std::vector<std::uint8_t> shadowed = ground_values(render_view(*renderer, stage, lit_sun, LightingView::ShadowFactor));
    CHECK(count_below(shadowed, 64) >= 10);
    CHECK(count_above(shadowed, 250) >= 100);
    CHECK(renderer->lighting_stats().shadow_maps == 4);
    CHECK(renderer->lighting_stats().shadow_maps_rendered == 4);
    CHECK(renderer->lighting_stats().shadow_maps_dropped == 0);
    CHECK(renderer->lighting_stats().shadow_draws >= 4);

    // The same sun without shadows casts none: every ground pixel is lit.
    const std::vector<Light> open_sun{sun(false)};
    const std::vector<std::uint8_t> unshadowed = ground_values(render_view(*renderer, stage, open_sun, LightingView::ShadowFactor));
    CHECK(count_below(unshadowed, 250) == 0);
    CHECK(renderer->lighting_stats().shadow_maps == 0);

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
}

TEST_CASE("DX12 directional shadows: the occluder shadows the ground, and an unshadowed sun does not", "[gpu][dx12]") {
    run_directional_shadow(Backend::Dx12);
}

TEST_CASE("Vulkan directional shadows: the occluder shadows the ground, and an unshadowed sun does not", "[gpu][vulkan]") {
    run_directional_shadow(Backend::Vulkan);
}

// ----- Shadow acne: the bias fixture -----

void run_acne(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto renderer = make_renderer(*device, small_desc(), backend);
    // No occluder: every ground pixel should be lit. Without bias, the ground shadows itself (acne); the tuned bias removes it.
    Stage stage = make_stage(false);
    upload_stage(*renderer, stage);

    const std::vector<Light> untuned{sun(true, 0.0f, 0.0f)};
    const std::vector<std::uint8_t> acne = ground_values(render_view(*renderer, stage, untuned, LightingView::ShadowFactor));
    CHECK(count_below(acne, 250) > 0);

    const std::vector<Light> tuned{sun(true)};
    const std::vector<std::uint8_t> clean = ground_values(render_view(*renderer, stage, tuned, LightingView::ShadowFactor));
    CHECK(count_below(clean, 250) == 0);

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
}

TEST_CASE("DX12 shadow acne: the zero-bias fixture shows self-shadowing and the tuned bias removes it", "[gpu][dx12]") {
    run_acne(Backend::Dx12);
}

TEST_CASE("Vulkan shadow acne: the zero-bias fixture shows self-shadowing and the tuned bias removes it", "[gpu][vulkan]") {
    run_acne(Backend::Vulkan);
}

// ----- Cascades -----

void run_cascades(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto renderer = make_renderer(*device, small_desc(), backend);
    Stage stage = make_stage(true);
    upload_stage(*renderer, stage);

    // The ground runs from the near field to the shadow distance, so it crosses several cascades. The cascade view colors each by the
    // cascade it used, so more than one color must appear on the ground.
    const std::vector<Light> lights{sun(true)};
    const SceneImage image = render_view(*renderer, stage, lights, LightingView::CascadeIndex);
    std::set<std::uint32_t> colors;
    for (std::size_t i = 0; i < image.pick_ids.size(); ++i) {
        if (image.pick_ids[i] == 1) {
            const auto& c = image.color[i];
            colors.insert(static_cast<std::uint32_t>(c[0]) << 16 | static_cast<std::uint32_t>(c[1]) << 8 | c[2]);
        }
    }
    CHECK(colors.size() >= 2);
    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
}

TEST_CASE("DX12 cascades: the ground is split between more than one cascade", "[gpu][dx12]") {
    run_cascades(Backend::Dx12);
}

TEST_CASE("Vulkan cascades: the ground is split between more than one cascade", "[gpu][vulkan]") {
    run_cascades(Backend::Vulkan);
}

// ----- Spot shadows, invalidation, and the atlas budget -----

void run_spot_and_invalidation(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto renderer = make_renderer(*device, small_desc(), backend);
    Stage stage = make_stage(true);
    upload_stage(*renderer, stage);

    // A spot light above and in front of the occluder casts its shadow onto the ground behind it.
    const std::vector<Light> spot{spot_at(7, glm::vec3(0.0f, 4.0f, -2.0f), glm::vec3(0.0f, -2.5f, -4.0f), true)};
    const std::vector<std::uint8_t> spot_shadow = ground_values(render_view(*renderer, stage, spot, LightingView::ShadowFactor));
    CHECK(count_below(spot_shadow, 64) >= 1);
    CHECK(renderer->lighting_stats().shadow_maps == 1);

    // Second frame, same scene: the tile's key is unchanged, so nothing is drawn into the atlas.
    static_cast<void>(render_view(*renderer, stage, spot, LightingView::ShadowFactor));
    CHECK(renderer->lighting_stats().shadow_maps_rendered == 0);

    // Moving the occluder changes a caster inside the tile's frustum, so the tile is drawn again. The meshes are already uploaded.
    Stage moved = make_stage(true, glm::dvec3(1.0, 2.5, -6.0));
    static_cast<void>(render_view(*renderer, moved, spot, LightingView::ShadowFactor));
    CHECK(renderer->lighting_stats().shadow_maps_rendered == 1);

    // Invalidation draws the light's tile without any change to it.
    renderer->invalidate_shadows(7);
    static_cast<void>(render_view(*renderer, moved, spot, LightingView::ShadowFactor));
    CHECK(renderer->lighting_stats().shadow_maps_rendered == 1);

    // Two spot lights, one tile of room: the second is dropped and counted, and its light still lights the scene.
    SceneRendererDesc tight = small_desc();
    tight.shadows.local_atlas_size = 256;
    tight.shadows.local_size = 256;
    auto tight_renderer = make_renderer(*device, tight, backend);
    upload_stage(*tight_renderer, stage);
    const std::vector<Light> two{spot_at(8, glm::vec3(0.0f, 4.0f, -2.0f), glm::vec3(0.0f, -2.5f, -4.0f), true),
                                 spot_at(9, glm::vec3(2.0f, 4.0f, -2.0f), glm::vec3(0.0f, -2.5f, -4.0f), true)};
    static_cast<void>(render_view(*tight_renderer, stage, two, LightingView::Lit));
    CHECK(tight_renderer->lighting_stats().shadow_maps == 1);
    CHECK(tight_renderer->lighting_stats().shadow_maps_dropped == 1);
    CHECK(tight_renderer->lighting_stats().lights == 2);

    require_ok(device->check_debug_messages());
    require_ok(tight_renderer->release());
    require_ok(renderer->release());
}

TEST_CASE("DX12 spot shadows: a spot light shadows the ground, redraws only on change, and drops a tile with no room", "[gpu][dx12]") {
    run_spot_and_invalidation(Backend::Dx12);
}

TEST_CASE("Vulkan spot shadows: a spot light shadows the ground, redraws only on change, and drops a tile with no room", "[gpu][vulkan]") {
    run_spot_and_invalidation(Backend::Vulkan);
}

// ----- A missing light -----

void run_missing_light(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto renderer = make_renderer(*device, small_desc(), backend);
    Stage stage = make_stage(false);
    upload_stage(*renderer, stage);
    // A dim environment, so the ground is not already at the top of the range and a light can be seen to add to it.
    engine::render::EquirectImage dim;
    dim.width = 16;
    dim.height = 8;
    dim.texels.assign(static_cast<std::size_t>(dim.width) * dim.height, glm::vec3(0.05f));
    require_ok(renderer->set_environment(dim));

    // Per pixel, not on the mean: a point light brightens only the ground near it, so the mean over the frame barely moves.
    const SceneImage without = render_view(*renderer, stage, {}, LightingView::Lit);
    const std::vector<Light> point{point_at(1, glm::vec3(0.0f, 2.0f, -4.0f), 12.0f)};
    const SceneImage with = render_view(*renderer, stage, point, LightingView::Lit);
    // Diagnostic: the light must reach the ground's clusters, or the shading can add nothing.
    const SceneImage clusters = render_view(*renderer, stage, point, LightingView::ClusterCount);
    std::size_t reached = 0;
    std::size_t brighter = 0;
    for (std::size_t i = 0; i < with.pick_ids.size(); ++i) {
        if (with.pick_ids[i] != 1) {
            continue;
        }
        reached += clusters.color[i][0] > 0 ? 1 : 0;
        brighter += with.color[i][0] > without.color[i][0] + 20 ? 1 : 0;
    }
    CHECK(reached > 0);
    CHECK(brighter >= 20);
    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
}

TEST_CASE("DX12 missing lights: a point light brightens the ground it reaches", "[gpu][dx12]") {
    run_missing_light(Backend::Dx12);
}

TEST_CASE("Vulkan missing lights: a point light brightens the ground it reaches", "[gpu][vulkan]") {
    run_missing_light(Backend::Vulkan);
}
