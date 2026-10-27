// Terrain on real GPUs (US-047). A 2 x 2 tile terrain is uploaded as static meshes and drawn through the scene renderer from above. Each run
// checks that the tiles cover the view with no holes, that each tile's entity owns its quadrant, and that the depth of the flat ground is
// right. A second frame mixes LOD 0 and LOD 1 across the shared seam. The fine tile stitches its edge onto the coarse one, so that frame
// checks the stitched seam in rasterized output. The renderable must also switch to the coarse mesh. The CPU reference rasterizer must
// agree with the GPU, and every run repeats its checks with the debug layers on. The tiles carry splat weights (even tiles grass, odd tiles
// snow): the colours must show the splat, which the flat ground's shading cannot hide. A third frame adds a shadow-casting sun, so the
// terrain shadow pipeline draws under the debug layers. A last frame raises a hill on the flat ground under a low sun in the ShadowFactor
// view: the hill's cast shadow must darken ground pixels that the open sun leaves lit, so the shadow pipeline must place the shadow on
// the terrain.

#include "render_scene_fixture.hpp"

#include "engine/core/result.hpp"
#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/reference_raster.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"
#include "engine/render/terrain_mesh.hpp"

#include "engine/rhi/device.hpp"
#include "engine/scene/scene_graph.hpp"
#include "engine/world/terrain.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using engine::render::Camera;
using engine::render::extract_frame;
using engine::render::CascadeSettings;
using engine::render::FrameSnapshot;
using engine::render::Light;
using engine::render::LightingView;
using engine::render::LightType;
using engine::render::make_terrain_render_tile;
using engine::render::MeshData;
using engine::render::pick_entity;
using engine::render::RenderableRef;
using engine::render::rasterize_reference;
using engine::render::SceneImage;
using engine::render::SceneRenderer;
using engine::render::SceneRendererDesc;
using engine::render::SceneShaders;
using engine::render::tests::golden_depth;
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
using engine::world::Terrain;
using engine::world::TerrainDesc;
using engine::world::TerrainTileMesh;

namespace {

using Catch::Matchers::WithinAbs;

template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

template <typename T>
T take_core(engine::core::Result<T, std::string> result) {
    if (!result.ok()) {
        FAIL(result.error());
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

// 2 x 2 tiles of 8 cells: a 16 m square of flat ground. Tile (x, z) is entity index z * 2 + x, as in the tile list.
constexpr std::uint32_t kTilesPerSide = 2;
constexpr std::uint32_t kTileCells = 8;
constexpr std::uint32_t kTileCount = kTilesPerSide * kTilesPerSide;

// A camera above the terrain's centre looking straight down. The 20 degree field keeps the whole view over the ground (about 5.3 m half
// width at 30 m height, inside the 8 m half extent), so every pixel must hit a tile.
Camera top_down_camera() {
    Camera camera;
    camera.position = glm::dvec3(8.0, 30.0, 8.0);
    camera.orientation = glm::angleAxis(-glm::half_pi<double>(), glm::dvec3(1.0, 0.0, 0.0));
    camera.fov_y_radians = glm::radians(20.0);
    camera.aspect = 1.0;
    camera.near_plane = 0.1;
    camera.far_plane = 1000.0;
    return camera;
}

Terrain make_flat_terrain() {
    TerrainDesc desc;
    desc.tiles_x = kTilesPerSide;
    desc.tiles_z = kTilesPerSide;
    desc.tile_cells = kTileCells;
    desc.cell_size = 1.0f;
    return take_core(engine::world::create_terrain(desc));
}

// Uploads each tile at its LOD and returns the mesh index of each tile, in tile order. Even tiles are pure grass (layer 0) and odd tiles
// pure snow (layer 3), so each tile's colour shows its own layer.
std::vector<std::uint32_t> upload_tiles(SceneRenderer& renderer, const Terrain& terrain, const std::vector<std::uint32_t>& lods) {
    const std::vector<TerrainTileMesh> tiles = take_core(engine::world::build_terrain_meshes(terrain, lods));
    std::vector<std::uint32_t> indices;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        engine::render::TerrainRenderTile render_tile = take_core(make_terrain_render_tile(tiles[i]));
        const glm::vec4 layer = i % 2 == 0 ? glm::vec4(1.0f, 0.0f, 0.0f, 0.0f) : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        std::fill(render_tile.splat.begin(), render_tile.splat.end(), layer);
        indices.push_back(take(renderer.upload_terrain_mesh(render_tile.mesh, render_tile.splat)));
    }
    return indices;
}

// A sun that casts shadows. A flat tile shades nothing of its own, so frame 3 checks that the terrain's shadow pipeline draws cleanly
// under the debug layers. Frame 4 checks the shadow's shape with a hill.
Light sun() {
    Light light;
    light.id = 1;
    light.type = LightType::Directional;
    light.direction = glm::normalize(glm::vec3(0.4f, -1.0f, -0.2f));
    light.color = glm::vec3(1.0f);
    light.intensity = 1.0f;
    light.casts_shadow = true;
    light.depth_bias = 0.0005f;
    light.light_bias = 1.0f;
    light.pcf_radius = 1;
    return light;
}

// A low sun from the -X side, so a raised hill shades the ground to its +X side. The direction is the light's travel direction.
Light low_sun(bool casts_shadow) {
    Light light = sun();
    light.direction = glm::normalize(glm::vec3(1.0f, -0.6f, 0.0f));
    light.casts_shadow = casts_shadow;
    return light;
}

// The flat terrain with a 4 m hill centred on the sample (6, 8), with a 3-sample radius. Its cosine falloff keeps the ground around it
// level, so the only shading change is the hill's shadow.
Terrain make_hill_terrain() {
    constexpr float height = 4.0f;
    constexpr float radius = 3.0f;
    Terrain terrain = make_flat_terrain();
    for (std::uint32_t iz = 0; iz < terrain.samples_z; ++iz) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ++ix) {
            const float distance = std::hypot(static_cast<float>(ix) - 6.0f, static_cast<float>(iz) - 8.0f);
            if (distance < radius) {
                const float falloff = 0.5f * (1.0f + std::cos(glm::pi<float>() * distance / radius));
                terrain.heights[engine::world::sample_index(terrain, ix, iz)] = height * falloff;
            }
        }
    }
    return terrain;
}

// Entities for the tiles: one node at the origin per tile, so the tile's world-space mesh is drawn at its place.
struct TerrainScene {
    engine::scene::SceneGraph graph;
    std::vector<engine::ecs::EntityHandle> tiles;
    std::vector<RenderableRef> renderables;
};

TerrainScene make_terrain_scene() {
    TerrainScene scene;
    for (std::uint32_t i = 0; i < kTileCount; ++i) {
        scene.tiles.push_back(scene.graph.create(engine::scene::Transform{}).value());
        RenderableRef renderable;
        renderable.node = scene.tiles.back();
        renderable.color = glm::vec4(1.0f);
        scene.renderables.push_back(renderable);
    }
    return scene;
}

// Points each tile's renderable at its uploaded mesh index for this LOD set.
void bind_tiles(TerrainScene& scene, const std::vector<std::uint32_t>& mesh_indices) {
    for (std::size_t i = 0; i < scene.renderables.size(); ++i) {
        scene.renderables[i].mesh = mesh_indices[i];
    }
}

// Pixels whose entity is not any tile: the terrain covers the view, so every pixel must be a tile and a zero ID is a hole. Also collects
// which tile entities own at least one pixel.
std::size_t count_holes(const SceneImage& image, const FrameSnapshot& snapshot, const TerrainScene& scene, std::vector<bool>& owned) {
    std::size_t holes = 0;
    owned.assign(kTileCount, false);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::optional<engine::ecs::EntityHandle> entity = pick_entity(image, snapshot, x, y);
            if (!entity.has_value()) {
                ++holes;
                continue;
            }
            for (std::uint32_t i = 0; i < kTileCount; ++i) {
                if (*entity == scene.tiles[i]) {
                    owned[i] = true;
                }
            }
        }
    }
    return holes;
}

// The first pixel that a tile owns, as an index into the colour array, if the tile owns any.
std::optional<std::size_t> first_pixel_of(const SceneImage& image, const FrameSnapshot& snapshot, const TerrainScene& scene, std::uint32_t tile) {
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::optional<engine::ecs::EntityHandle> entity = pick_entity(image, snapshot, x, y);
            if (entity.has_value() && *entity == scene.tiles[tile]) {
                return image.index(x, y);
            }
        }
    }
    return std::nullopt;
}

// Pixels where the GPU's entity ID differs from the CPU reference. The reference may disagree on a pixel whose centre lies on a triangle
// edge, so the check allows a few such pixels, as the scene fixture does.
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

// The scene shaders, the terrain pair, and the ray-traced effects (US-043), which the traced shadow case needs.
SceneShaders terrain_shaders(Backend backend) {
    SceneShaders shaders;
    shaders.vertex = ShaderBinary{ShaderStage::Vertex, "VSMain", load_shader(backend, "scene_vertex")};
    shaders.pixel = ShaderBinary{ShaderStage::Pixel, "PSMain", load_shader(backend, "scene_pixel")};
    shaders.pixel_blend = ShaderBinary{ShaderStage::Pixel, "PSBlend", load_shader(backend, "scene_pixel_blend")};
    shaders.shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSShadow", load_shader(backend, "scene_shadow_vertex")};
    shaders.shadow_pixel = ShaderBinary{ShaderStage::Pixel, "PSShadow", load_shader(backend, "scene_shadow_pixel")};
    shaders.indirect_vertex = ShaderBinary{ShaderStage::Vertex, "VSIndirect", load_shader(backend, "scene_indirect_vertex")};
    shaders.cull = ShaderBinary{ShaderStage::Compute, "CSCull", load_shader(backend, "scene_cull")};
    shaders.terrain_vertex = ShaderBinary{ShaderStage::Vertex, "VSTerrain", load_shader(backend, "scene_terrain_vertex")};
    shaders.terrain_shadow_vertex = ShaderBinary{ShaderStage::Vertex, "VSTerrainShadow", load_shader(backend, "scene_terrain_shadow_vertex")};
    shaders.ray_traced = ShaderBinary{ShaderStage::Compute, "CSRayTracedEffects", load_shader(backend, "rt_effects")};
    return shaders;
}

void run_terrain_scene(Backend backend) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    const auto baseline = take(device->resource_stats());

    SceneRendererDesc desc;
    desc.width = kSceneWidth;
    desc.height = kSceneHeight;
    auto renderer = take(SceneRenderer::create(*device, desc, terrain_shaders(backend)));

    const Terrain terrain = make_flat_terrain();
    const Camera camera = top_down_camera();
    TerrainScene scene = make_terrain_scene();

    // Frame 1: every tile at LOD 0. The view is covered, every quadrant is owned by its tile, and the ground depth is the camera height.
    const std::vector<std::uint32_t> lod0_meshes = upload_tiles(*renderer, terrain, {0, 0, 0, 0});
    bind_tiles(scene, lod0_meshes);
    {
        const FrameSnapshot snapshot = extract_frame(scene.graph, camera, renderer->bounds(), scene.renderables);
        CHECK(snapshot.stats().drawn == kTileCount);
        require_ok(renderer->render(snapshot));
        std::vector<bool> owned;
        CHECK(count_holes(renderer->image(), snapshot, scene, owned) == 0);
        for (std::uint32_t i = 0; i < kTileCount; ++i) {
            INFO("tile " << i << " owns no pixel");
            CHECK(owned[i]);
        }
        CHECK_THAT(static_cast<double>(renderer->image().depth[renderer->image().index(32, 32)]), WithinAbs(golden_depth(30.0), 1e-4));
        // The splat tints the albedo: snow (tile 1) reads brighter than grass (tile 0) in every channel, and grass is green-dominant. The
        // ground is flat and lit alike under both, so only the layer colour separates them.
        const std::optional<std::size_t> grass = first_pixel_of(renderer->image(), snapshot, scene, 0);
        const std::optional<std::size_t> snow = first_pixel_of(renderer->image(), snapshot, scene, 1);
        REQUIRE(grass.has_value());
        REQUIRE(snow.has_value());
        const std::array<std::uint8_t, 4>& grass_colour = renderer->image().color[*grass];
        const std::array<std::uint8_t, 4>& snow_colour = renderer->image().color[*snow];
        INFO("grass " << int(grass_colour[0]) << " " << int(grass_colour[1]) << " " << int(grass_colour[2]) << ", snow " << int(snow_colour[0])
                      << " " << int(snow_colour[1]) << " " << int(snow_colour[2]));
        CHECK(grass_colour[1] > grass_colour[0]);
        CHECK(grass_colour[1] > grass_colour[2]);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            CHECK(snow_colour[channel] > grass_colour[channel]);
        }
        check_against_reference(renderer->image(), snapshot, renderer->meshes());
    }

    // Frame 2: the tiles at x = 1 move to LOD 1 while x = 0 stays at LOD 0. The seam at x = 8 is in view, so the fine tile on its -X side
    // stitches onto the coarse tile on its +X side. The renderables switch to the coarse uploads and the seam must still have no holes.
    const std::vector<std::uint32_t> mixed_lods = {0, 1, 0, 1};
    const std::vector<std::uint32_t> mixed_meshes = upload_tiles(*renderer, terrain, mixed_lods);
    bind_tiles(scene, mixed_meshes);
    {
        const FrameSnapshot snapshot = extract_frame(scene.graph, camera, renderer->bounds(), scene.renderables);
        CHECK(snapshot.stats().drawn == kTileCount);
        require_ok(renderer->render(snapshot));
        std::vector<bool> owned;
        CHECK(count_holes(renderer->image(), snapshot, scene, owned) == 0);
        for (std::uint32_t i = 0; i < kTileCount; ++i) {
            INFO("tile " << i << " owns no pixel");
            CHECK(owned[i]);
        }
        // An 8-cell tile has 384 indices at LOD 0 and 4-cell tiles have 96 at LOD 1, so each renderable must hold the mesh of its LOD.
        for (std::uint32_t i = 0; i < kTileCount; ++i) {
            const std::uint32_t expected = mixed_lods[i] == 0 ? 384u : 96u;
            INFO("tile " << i << " mesh index count");
            CHECK(renderer->meshes()[scene.renderables[i].mesh].indices.size() == expected);
        }
        check_against_reference(renderer->image(), snapshot, renderer->meshes());
    }

    // Frame 3: the mixed LODs again, now under a shadow-casting sun. The terrain shadow pipeline draws every tile into the shadow atlas, and
    // the view must still have no holes and keep each tile's entity.
    {
        const std::vector<Light> lights{sun()};
        const CascadeSettings cascades = desc.cascades;
        const FrameSnapshot snapshot = extract_frame(scene.graph, camera, renderer->bounds(), scene.renderables, lights, cascades);
        CHECK(snapshot.stats().drawn == kTileCount);
        require_ok(renderer->render(snapshot));
        std::vector<bool> owned;
        CHECK(count_holes(renderer->image(), snapshot, scene, owned) == 0);
        for (std::uint32_t i = 0; i < kTileCount; ++i) {
            INFO("tile " << i << " owns no pixel");
            CHECK(owned[i]);
        }
    }

    // Frame 4: a hill on the flat ground under a low sun, in the ShadowFactor view (1 lit, 0 shadowed). The open sun leaves every terrain
    // pixel lit. With the shadow-casting sun, the hill's shadow darkens some terrain pixels and leaves most of the view lit.
    {
        const Terrain hill = make_hill_terrain();
        bind_tiles(scene, upload_tiles(*renderer, hill, {0, 0, 0, 0}));
        renderer->set_lighting_view(LightingView::ShadowFactor);
        const std::vector<Light> open{low_sun(false)};
        const FrameSnapshot open_snapshot =
            extract_frame(scene.graph, camera, renderer->bounds(), scene.renderables, open, desc.cascades);
        require_ok(renderer->render(open_snapshot));
        std::size_t open_shadowed = 0;
        for (std::uint32_t y = 0; y < renderer->image().height; ++y) {
            for (std::uint32_t x = 0; x < renderer->image().width; ++x) {
                const bool terrain_pixel = pick_entity(renderer->image(), open_snapshot, x, y).has_value();
                if (terrain_pixel && renderer->image().color[renderer->image().index(x, y)][0] < 250) {
                    ++open_shadowed;
                }
            }
        }
        CHECK(open_shadowed == 0);

        const std::vector<Light> shadowing{low_sun(true)};
        const FrameSnapshot snapshot = extract_frame(scene.graph, camera, renderer->bounds(), scene.renderables, shadowing, desc.cascades);
        require_ok(renderer->render(snapshot));
        std::vector<bool> owned;
        CHECK(count_holes(renderer->image(), snapshot, scene, owned) == 0);
        std::size_t shadowed = 0;
        std::size_t lit = 0;
        for (std::uint32_t y = 0; y < renderer->image().height; ++y) {
            for (std::uint32_t x = 0; x < renderer->image().width; ++x) {
                if (!pick_entity(renderer->image(), snapshot, x, y).has_value()) {
                    continue;
                }
                const std::uint8_t value = renderer->image().color[renderer->image().index(x, y)][0];
                shadowed += value < 64 ? 1 : 0;
                lit += value > 250 ? 1 : 0;
            }
        }
        INFO("shadowed terrain pixels " << shadowed << ", lit " << lit);
        CHECK(shadowed >= 20);
        CHECK(lit >= 200);
        renderer->set_lighting_view(LightingView::Lit);
    }

    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    const auto after = take(device->resource_stats());
    CHECK(after.buffers == baseline.buffers);
    CHECK(after.textures == baseline.textures);
    CHECK(after.descriptor_sets == baseline.descriptor_sets);
    require_ok(device->check_debug_messages());
}

// Renders the hill under the low sun with the ray-traced shadows on or off (US-043). The sun casts no shadow map, so with the pass on the
// only shadow is the traced one. Reports whether the pass ran and how many tiles it traced.
SceneImage render_hill(Backend backend, bool ray_traced, bool& active, std::uint32_t& traced, LightingView view = LightingView::Lit) {
    auto device = take(engine::rhi::create_device(debug_device(backend)));
    SceneRendererDesc desc;
    desc.width = kSceneWidth;
    desc.height = kSceneHeight;
    desc.ray_traced.enabled = ray_traced;
    desc.ray_traced.roughness_cutoff = 0.4f;
    desc.ray_traced.reflection_distance = 100.0f;
    desc.ray_traced.shadow_distance = 1000.0f;
    auto renderer = take(SceneRenderer::create(*device, desc, terrain_shaders(backend)));
    renderer->set_lighting_view(view);
    // A black environment, so the sun alone lights the ground. The default radiance of one fills a shadow and leaves the colour saturated.
    engine::render::EquirectImage environment;
    environment.width = 16;
    environment.height = 8;
    environment.texels.assign(static_cast<std::size_t>(environment.width) * environment.height, glm::vec3(0.0f));
    require_ok(renderer->set_environment(environment));

    TerrainScene scene = make_terrain_scene();
    bind_tiles(scene, upload_tiles(*renderer, make_hill_terrain(), {0, 0, 0, 0}));
    const std::vector<Light> lights{low_sun(false)};
    const FrameSnapshot snapshot = extract_frame(scene.graph, top_down_camera(), renderer->bounds(), scene.renderables, lights, desc.cascades, true);
    require_ok(renderer->render(snapshot));
    active = renderer->ray_traced_active();
    traced = renderer->ray_traced_instances();
    SceneImage image = renderer->image();
    require_ok(device->check_debug_messages());
    require_ok(renderer->release());
    return image;
}

// The sum of a pixel's red, green, and blue values: a brightness the traced shadow can lower.
int luminance_sum(const std::array<std::uint8_t, 4>& colour) {
    return static_cast<int>(colour[0]) + static_cast<int>(colour[1]) + static_cast<int>(colour[2]);
}

// The traced shadow darkens terrain pixels that the same frame without the pass leaves lit, and never lights one it leaves dark.
void run_terrain_traced_shadow(Backend backend) {
    bool traced_active = false;
    std::uint32_t traced_tiles = 0;
    const SceneImage traced = render_hill(backend, true, traced_active, traced_tiles);
    CHECK(traced_active);
    CHECK(traced_tiles == kTileCount);

    bool raster_active = true;
    std::uint32_t raster_tiles = 0;
    const SceneImage raster = render_hill(backend, false, raster_active, raster_tiles);
    CHECK_FALSE(raster_active);

    // The ground is lit by the sun alone (no environment), so a lit pixel sums to a few tens of levels. A shadowed pixel keeps under half of
    // that, so the check is relative to the pass-off value rather than a fixed step.
    std::size_t darkened = 0;
    std::size_t brightened = 0;
    int brightest_raster = 0;
    for (std::size_t i = 0; i < traced.pick_ids.size(); ++i) {
        if (traced.pick_ids[i] == 0) {
            continue;
        }
        const int traced_sum = luminance_sum(traced.color[i]);
        const int raster_sum = luminance_sum(raster.color[i]);
        brightest_raster = std::max(brightest_raster, raster_sum);
        darkened += raster_sum >= 12 && 2 * traced_sum <= raster_sum ? 1 : 0;
        brightened += traced_sum > raster_sum + 6 ? 1 : 0;
    }
    // The ShadowFactor view shows the traced term the lit pass reads (grey 255 lit, 0 shadowed), so it checks that the same shadow reaches
    // the lighting and not only the colour.
    bool factor_active = false;
    std::uint32_t factor_tiles = 0;
    const SceneImage factor = render_hill(backend, true, factor_active, factor_tiles, LightingView::ShadowFactor);
    std::size_t factor_shadowed = 0;
    for (std::size_t i = 0; i < factor.pick_ids.size(); ++i) {
        factor_shadowed += factor.pick_ids[i] != 0 && factor.color[i][0] < 128 ? 1 : 0;
    }
    INFO("terrain pixels darkened by the traced shadow " << darkened << ", brightened " << brightened << ", brightest pass-off sum "
                                                          << brightest_raster << ", shadowed in the ShadowFactor view " << factor_shadowed);
    CHECK(darkened >= 20);
    CHECK(brightened <= 4);
    CHECK(factor_shadowed >= 20);
}

}  // namespace

TEST_CASE("DX12 draws tiled terrain: coverage, tile picking, depth, and an LOD switch across a stitched seam", "[gpu][dx12]") {
    run_terrain_scene(Backend::Dx12);
}

TEST_CASE("Vulkan draws tiled terrain: coverage, tile picking, depth, and an LOD switch across a stitched seam", "[gpu][vulkan]") {
    run_terrain_scene(Backend::Vulkan);
}

TEST_CASE("DX12 terrain casts a traced shadow: the pass darkens the ground the hill covers", "[gpu][dx12]") {
    run_terrain_traced_shadow(Backend::Dx12);
}

TEST_CASE("Vulkan terrain casts a traced shadow: the pass darkens the ground the hill covers", "[gpu][vulkan]") {
    run_terrain_traced_shadow(Backend::Vulkan);
}
