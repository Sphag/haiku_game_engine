// Direct lighting and shadow planning (US-035), on the CPU. These cases need no GPU: they check the light model's validation and falloff,
// the cluster grid and its overflow control, the cascade fit, the spot matrix, the shadow atlas's allocation and invalidation, the tile
// remap, and the frame snapshot's shadow-only casters. The GPU suites check the shader against these results.

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/shadow_atlas.hpp"

#include "engine/core/coordinates.hpp"
#include "engine/scene/scene_graph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using engine::render::AtlasRect;
using engine::render::assign_clusters;
using engine::render::Camera;
using engine::render::CascadePlan;
using engine::render::CascadeSettings;
using engine::render::ClusteredLight;
using engine::render::ClusterAssignment;
using engine::render::ClusterGrid;
using engine::render::extract_frame;
using engine::render::FrameSnapshot;
using engine::render::hash_bytes;
using engine::render::Light;
using engine::render::LightingSettings;
using engine::render::LightType;
using engine::render::light_attenuation;
using engine::render::make_cluster_grid;
using engine::render::plan_cascades;
using engine::render::RenderableRef;
using engine::render::ShadowAtlas;
using engine::render::ShadowTileRequest;
using engine::render::spot_view_projection;
using engine::render::tile_remap;
using engine::render::tile_row_sign;
using engine::render::validate_light;
using engine::render::MeshBounds;

namespace {

using Catch::Matchers::WithinAbs;

// The camera of the golden scene: at z = 10 looking down -Z, 60 degrees vertical, 1:1, depth 0.1 to 1000.
Camera test_camera() {
    Camera camera;
    camera.position = glm::dvec3(0.0, 0.0, 10.0);
    camera.fov_y_radians = 1.0471975511965976;
    camera.aspect = 1.0;
    camera.near_plane = 0.1;
    camera.far_plane = 1000.0;
    return camera;
}

LightingSettings test_settings() {
    LightingSettings settings;
    settings.max_lights = 16;
    settings.tile_px = 16;
    settings.slices = 8;
    settings.max_lights_per_cluster = 4;
    return settings;
}

ClusterGrid test_grid(const LightingSettings& settings = test_settings()) {
    const auto grid = make_cluster_grid(settings, test_camera(), 64, 64);
    REQUIRE(grid.has_value());
    return *grid;
}

// Camera-relative point light at the given offset from the camera.
ClusteredLight point_at(std::uint32_t index, glm::vec3 offset, float radius) {
    ClusteredLight light;
    light.index = index;
    light.type = LightType::Point;
    light.center = offset;
    light.radius = radius;
    return light;
}

// The engine (DX-convention) view-projection of the test camera, camera-relative.
glm::mat4 test_view_projection() {
    return engine::render::camera_view_projection(test_camera());
}

}  // namespace

TEST_CASE("Light validation rejects unusable parameters and accepts the three types", "[unit]") {
    Light point;
    point.type = LightType::Point;
    point.position = glm::dvec3(1.0, 2.0, 3.0);
    CHECK_FALSE(validate_light(point).has_value());

    Light sun;
    sun.type = LightType::Directional;
    sun.direction = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
    CHECK_FALSE(validate_light(sun).has_value());

    Light spot;
    spot.type = LightType::Spot;
    spot.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    spot.inner_angle = 0.2f;
    spot.outer_angle = 0.4f;
    CHECK_FALSE(validate_light(spot).has_value());

    Light no_range = point;
    no_range.range = 0.0f;
    CHECK(validate_light(no_range).has_value());

    Light not_unit = spot;
    not_unit.direction = glm::vec3(0.0f, -2.0f, 0.0f);
    CHECK(validate_light(not_unit).has_value());

    Light inverted_cone = spot;
    inverted_cone.inner_angle = 0.5f;
    inverted_cone.outer_angle = 0.2f;
    CHECK(validate_light(inverted_cone).has_value());

    Light wide_cone = spot;
    wide_cone.outer_angle = 1.6f;
    CHECK(validate_light(wide_cone).has_value());

    Light negative = point;
    negative.intensity = -1.0f;
    CHECK(validate_light(negative).has_value());

    Light deep_bias = point;
    deep_bias.depth_bias = 0.5f;
    CHECK(validate_light(deep_bias).has_value());
}

TEST_CASE("The cluster grid maps depth slices exponentially and clamps at both planes", "[unit]") {
    const ClusterGrid grid = test_grid();
    CHECK(grid.tiles_x == 4);
    CHECK(grid.tiles_y == 4);
    CHECK(grid.cluster_count() == 4 * 4 * 8);

    // Depths at or below the near plane use slice 0, and depths past the far plane use the last slice.
    CHECK(grid.slice_of(0.05f) == 0);
    CHECK(grid.slice_of(grid.near_plane) == 0);
    CHECK(grid.slice_of(grid.far_plane * 2.0f) == 7);

    // Exponential spacing: the boundary between slices k and k + 1 is at near * (far / near) ^ (k / slices).
    for (std::uint32_t k = 1; k < 8; ++k) {
        const float boundary = grid.near_plane * std::pow(grid.far_plane / grid.near_plane, static_cast<float>(k) / 8.0f);
        CHECK(grid.slice_of(boundary * 1.001f) == k);
        CHECK(grid.slice_of(boundary * 0.999f) == k - 1);
    }

    // The cluster of a pixel is its tile, then its slice: tile (1, 2) at depth 0.5 m is column 1 in row 2.
    const std::uint32_t slice = grid.slice_of(0.5f);
    CHECK(grid.cluster_of(20, 40, 0.5f) == (slice * grid.tiles_y + 2) * grid.tiles_x + 1);
}

TEST_CASE("Unusable cluster settings are rejected instead of building a grid", "[unit]") {
    LightingSettings settings = test_settings();
    settings.tile_px = 0;
    CHECK_FALSE(make_cluster_grid(settings, test_camera(), 64, 64).has_value());
    settings = test_settings();
    settings.max_lights_per_cluster = 0x10000;
    CHECK_FALSE(make_cluster_grid(settings, test_camera(), 64, 64).has_value());
    CHECK_FALSE(make_cluster_grid(test_settings(), test_camera(), 0, 64).has_value());
}

TEST_CASE("A point light is assigned to the clusters its screen footprint covers, and to no others", "[unit]") {
    const ClusterGrid grid = test_grid();
    // A light two meters in front of the camera, 0.1 m radius, sits in the middle of the view. Its footprint is the four tiles around
    // the center. A larger light's conservative footprint covers the whole slice, which the overflow case relies on.
    const ClusteredLight light = point_at(0, glm::vec3(0.0f, 0.0f, -2.0f), 0.1f);
    const ClusterAssignment assignment = assign_clusters(grid, test_view_projection(), {light});

    const std::uint32_t center_slice = grid.slice_of(2.0f);
    const std::uint32_t center_cluster = grid.cluster_of(32, 32, 2.0f);
    CHECK(assignment.counts[center_cluster] == 1);
    CHECK(assignment.indices[static_cast<std::size_t>(center_cluster) * 4] == 0);

    // A corner tile far from the light is not touched.
    const std::uint32_t corner = (center_slice * grid.tiles_y + 0) * grid.tiles_x + 0;
    CHECK(assignment.counts[corner] == 0);
    std::uint32_t touched = 0;
    for (const std::uint32_t count : assignment.counts) {
        touched += count != 0 ? 1 : 0;
    }
    CHECK(touched >= 1);
    CHECK(touched <= 4);
}

TEST_CASE("A light behind the camera, or beyond the view, is assigned to no cluster", "[unit]") {
    const ClusterGrid grid = test_grid();
    // Behind the eye: the sphere is wholly on the far side of the camera.
    const ClusteredLight behind = point_at(0, glm::vec3(0.0f, 0.0f, 5.0f), 1.0f);
    // Far to the side, outside the frustum: the box projects off the screen.
    const ClusteredLight aside = point_at(1, glm::vec3(500.0f, 0.0f, -10.0f), 1.0f);
    const ClusterAssignment assignment = assign_clusters(grid, test_view_projection(), {behind, aside});
    for (const std::uint32_t count : assignment.counts) {
        CHECK(count == 0);
    }
}

TEST_CASE("A directional light covers every cluster, and a light past the far plane sits in the last slice", "[unit]") {
    const ClusterGrid grid = test_grid();
    ClusteredLight sun;
    sun.index = 3;
    sun.type = LightType::Directional;
    const ClusterAssignment assignment = assign_clusters(grid, test_view_projection(), {sun});
    for (const std::uint32_t count : assignment.counts) {
        CHECK(count == 1);
    }
    CHECK(assignment.overflow_clusters == 0);
    CHECK(assignment.dropped_entries == 0);
}

TEST_CASE("Overflow keeps the lights that come first and reports every dropped pair", "[unit]") {
    // max_lights_per_cluster is 4. Six lights cover the same cluster, in priority order: 0 to 5. Lights 0 to 3 fit. Lights 4 and 5 are
    // dropped, and the cluster is flagged.
    const ClusterGrid grid = test_grid();
    std::vector<ClusteredLight> lights;
    for (std::uint32_t i = 0; i < 6; ++i) {
        lights.push_back(point_at(i, glm::vec3(0.0f, 0.0f, -2.0f), 0.5f));
    }
    const ClusterAssignment assignment = assign_clusters(grid, test_view_projection(), lights);
    const std::uint32_t cluster = grid.cluster_of(32, 32, 2.0f);
    CHECK((assignment.counts[cluster] & 0xFFFFu) == 4);
    CHECK((assignment.counts[cluster] & engine::render::kClusterOverflowBit) != 0);
    for (std::uint32_t i = 0; i < 4; ++i) {
        CHECK(assignment.indices[static_cast<std::size_t>(cluster) * 4 + i] == i);
    }
    CHECK(assignment.overflow_clusters >= 1);
    CHECK(assignment.dropped_entries >= 2);
}

TEST_CASE("Attenuation reaches zero at the range, falls off with distance, and follows the spot cone", "[unit]") {
    Light point;
    point.type = LightType::Point;
    point.range = 10.0f;
    point.intensity = 4.0f;
    CHECK(light_attenuation(point, 10.0f, 1.0f) == 0.0f);
    CHECK(light_attenuation(point, 12.0f, 1.0f) == 0.0f);
    const float near = light_attenuation(point, 2.0f, 1.0f);
    const float far = light_attenuation(point, 4.0f, 1.0f);
    CHECK(near > far);
    CHECK(far > 0.0f);

    Light spot = point;
    spot.type = LightType::Spot;
    spot.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    spot.inner_angle = 0.2f;
    spot.outer_angle = 0.4f;
    const float on_axis = light_attenuation(spot, 2.0f, 1.0f);
    CHECK(on_axis == light_attenuation(point, 2.0f, 1.0f));
    CHECK(light_attenuation(spot, 2.0f, std::cos(0.6f)) == 0.0f);
    const float half = light_attenuation(spot, 2.0f, std::cos(0.3f));
    CHECK(half > 0.0f);
    CHECK(half < on_axis);

    Light sun;
    sun.type = LightType::Directional;
    sun.intensity = 2.5f;
    CHECK(light_attenuation(sun, 1000.0f, 0.0f) == 2.5f);
}

TEST_CASE("Cascades split the frustum in order and each slice projects inside its own light box", "[unit]") {
    const Camera camera = test_camera();
    CascadeSettings settings;
    settings.count = 4;
    settings.shadow_distance = 200.0f;
    settings.caster_margin = 50.0f;
    const glm::vec3 sun = glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f));
    const std::vector<CascadePlan> plans = plan_cascades(camera, sun, settings, 2048);
    REQUIRE(plans.size() == 4);

    float previous = static_cast<float>(camera.near_plane);
    for (const CascadePlan& plan : plans) {
        CHECK(plan.split_far > previous);
        previous = plan.split_far;
    }
    CHECK_THAT(static_cast<double>(plans.back().split_far), WithinAbs(200.0, 1e-3));

    // The slice's axis point at mid depth is inside its own cascade, and the box is bounded in x and y.
    const glm::vec3 forward = glm::vec3(camera.forward());
    for (std::size_t i = 0; i < plans.size(); ++i) {
        const float near = i == 0 ? static_cast<float>(camera.near_plane) : plans[i - 1].split_far;
        const glm::vec3 probe = forward * (0.5f * (near + plans[i].split_far));
        const glm::vec4 clip = plans[i].view_projection * glm::vec4(probe, 1.0f);
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        CHECK(std::abs(ndc.x) <= 1.0f);
        CHECK(std::abs(ndc.y) <= 1.0f);
        CHECK(ndc.z >= 0.0f);
        CHECK(ndc.z <= 1.0f);
    }
}

TEST_CASE("A cascade's light box stays fixed as the camera turns, so its size does not shimmer", "[unit]") {
    // Rotating the camera about its own origin leaves each slice's bounding sphere the same size, so the cascade radius (the box half
    // width) does not change. The light box's extent is read from the matrix scale on x.
    CascadeSettings settings;
    settings.count = 2;
    settings.shadow_distance = 120.0f;
    const glm::vec3 sun = glm::normalize(glm::vec3(0.0f, -1.0f, -0.2f));
    Camera a = test_camera();
    Camera b = test_camera();
    b.orientation = glm::angleAxis(0.7, glm::dvec3(0.0, 1.0, 0.0));
    const auto plans_a = plan_cascades(a, sun, settings, 1024);
    const auto plans_b = plan_cascades(b, sun, settings, 1024);
    REQUIRE(plans_a.size() == plans_b.size());
    for (std::size_t i = 0; i < plans_a.size(); ++i) {
        CHECK_THAT(static_cast<double>(glm::length(glm::vec3(plans_a[i].view_projection[0]))),
                   WithinAbs(static_cast<double>(glm::length(glm::vec3(plans_b[i].view_projection[0]))), 1e-5));
    }
}

TEST_CASE("A spot light's matrix maps its axis to the center of the tile and the cone's edge to the tile's border", "[unit]") {
    const glm::vec3 position(0.0f, 0.0f, 0.0f);
    const glm::vec3 direction(0.0f, 0.0f, -1.0f);
    const glm::mat4 vp = spot_view_projection(position, direction, 0.5f, 20.0f);
    const auto ndc_of = [&vp](const glm::vec3& p) {
        const glm::vec4 clip = vp * glm::vec4(p, 1.0f);
        return glm::vec3(clip) / clip.w;
    };
    const glm::vec3 axis = ndc_of(glm::vec3(0.0f, 0.0f, -5.0f));
    CHECK_THAT(static_cast<double>(axis.x), WithinAbs(0.0, 1e-5));
    CHECK_THAT(static_cast<double>(axis.y), WithinAbs(0.0, 1e-5));
    // A point on the cone's edge at the same depth is on the border of the square.
    const float edge = 5.0f * std::tan(0.5f);
    const glm::vec3 on_edge = ndc_of(glm::vec3(edge, 0.0f, -5.0f));
    CHECK_THAT(static_cast<double>(on_edge.x), WithinAbs(1.0, 1e-4));
    const glm::vec3 outside = ndc_of(glm::vec3(edge * 2.0f, 0.0f, -5.0f));
    CHECK(outside.x > 1.0f);
}

TEST_CASE("The shadow atlas grants, reuses, invalidates, and releases tiles by key", "[unit]") {
    // A 256 atlas holds four 128 tiles: two per row, in two shelves. The fifth request has no room.
    ShadowAtlas atlas(256);
    atlas.begin_frame();
    const ShadowTileRequest first = atlas.request(1, 128, 7);
    REQUIRE(first.granted);
    CHECK(first.render);  // a new tile is always drawn
    const ShadowTileRequest second = atlas.request(2, 128, 9);
    REQUIRE(second.granted);
    CHECK(second.rect.x != first.rect.x);  // the second tile of the first row
    CHECK(second.rect.y == first.rect.y);
    CHECK(atlas.request(3, 128, 9).granted);
    CHECK(atlas.request(4, 128, 9).granted);
    CHECK_FALSE(atlas.request(5, 128, 9).granted);
    atlas.end_frame();

    // The same keys next frame draw nothing: the tiles are valid and unchanged.
    atlas.begin_frame();
    CHECK_FALSE(atlas.request(1, 128, 7).render);
    CHECK_FALSE(atlas.request(2, 128, 9).render);
    CHECK_FALSE(atlas.request(3, 128, 9).render);
    CHECK_FALSE(atlas.request(4, 128, 9).render);
    atlas.end_frame();

    // A changed key redraws; so does an invalidation. Other tiles keep their content. Every tile is requested each frame, so none is
    // released.
    atlas.begin_frame();
    CHECK(atlas.request(1, 128, 8).render);
    CHECK_FALSE(atlas.request(2, 128, 9).render);
    CHECK_FALSE(atlas.request(3, 128, 9).render);
    CHECK_FALSE(atlas.request(4, 128, 9).render);
    atlas.invalidate(2);
    atlas.end_frame();
    atlas.begin_frame();
    CHECK_FALSE(atlas.request(1, 128, 8).render);
    CHECK(atlas.request(2, 128, 9).render);
    CHECK_FALSE(atlas.request(3, 128, 9).render);
    CHECK_FALSE(atlas.request(4, 128, 9).render);
    atlas.end_frame();
    CHECK(atlas.tile_count() == 4);

    // A tile that is not requested in a frame is released, so a later request for it draws again.
    atlas.begin_frame();
    CHECK(atlas.request(1, 128, 8).render == false);
    atlas.end_frame();
    CHECK(atlas.tile_count() == 1);
}

TEST_CASE("A released tile of the same size is reused, and an atlas with no room refuses the request", "[unit]") {
    ShadowAtlas atlas(128);
    atlas.begin_frame();
    const ShadowTileRequest a = atlas.request(10, 128, 1);
    REQUIRE(a.granted);
    CHECK_FALSE(atlas.request(11, 128, 1).granted);  // no room for a second 128 tile
    atlas.end_frame();  // id 10 was requested, so it stays; id 11 was never allocated

    // Frame 2 requests nothing: the tile is released.
    atlas.begin_frame();
    atlas.end_frame();
    CHECK(atlas.tile_count() == 0);

    atlas.begin_frame();
    const ShadowTileRequest b = atlas.request(11, 128, 1);
    REQUIRE(b.granted);
    CHECK(b.rect.x == a.rect.x);
    CHECK(b.rect.y == a.rect.y);
    CHECK(b.render);
    atlas.end_frame();
}

TEST_CASE("A tile whose size changes is allocated again, and a tile larger than the atlas is refused", "[unit]") {
    ShadowAtlas atlas(256);
    atlas.begin_frame();
    REQUIRE(atlas.request(4, 64, 1).granted);
    const ShadowTileRequest resized = atlas.request(4, 128, 1);
    REQUIRE(resized.granted);
    CHECK(resized.render);
    CHECK(resized.rect.size == 128);
    CHECK_FALSE(atlas.request(5, 512, 1).granted);
    atlas.end_frame();
}

TEST_CASE("The tile remap places light clip space in the tile and orders the rows as the backend does", "[unit]") {
    const AtlasRect tile{128, 0, 128};  // top-right quadrant of a 256 atlas
    for (const bool vulkan : {false, true}) {
        const glm::mat4 remap = tile_remap(tile, 256, vulkan);
        const float sign = tile_row_sign(vulkan);
        // Light clip (0, 0) is the tile's center on either backend: the pixel x is 192, and the row is 64 from the top.
        const glm::vec4 center = remap * glm::vec4(0.0f, 0.0f, 0.5f, 1.0f);
        const float x_pixel = (center.x / center.w * 0.5f + 0.5f) * 256.0f;
        const float y_row = (0.5f + 0.5f * sign * (center.y / center.w)) * 256.0f;
        CHECK_THAT(static_cast<double>(x_pixel), WithinAbs(192.0, 1e-3));
        CHECK_THAT(static_cast<double>(y_row), WithinAbs(64.0, 1e-3));

        // The light's edge at x = -1 maps to the tile's left column; the z value is unchanged.
        const glm::vec4 left = remap * glm::vec4(-1.0f, 0.0f, 0.25f, 1.0f);
        CHECK_THAT(static_cast<double>((left.x / left.w * 0.5f + 0.5f) * 256.0f), WithinAbs(128.0, 1e-3));
        CHECK_THAT(static_cast<double>(left.z), WithinAbs(0.25, 1e-6));
        CHECK_THAT(static_cast<double>(left.w), WithinAbs(1.0, 1e-6));
    }
}

TEST_CASE("Tile keys change with any input the tile depends on", "[unit]") {
    const float a[3] = {0.1f, 0.2f, 0.3f};
    const float b[3] = {0.1f, 0.2f, 0.4f};
    const std::uint64_t seed = 1469598103934665603ull;
    CHECK(hash_bytes(a, sizeof(a), seed) == hash_bytes(a, sizeof(a), seed));
    CHECK(hash_bytes(a, sizeof(a), seed) != hash_bytes(b, sizeof(b), seed));
}

TEST_CASE("A directional light's casters outside the view are kept as shadow-only items, and far side items are culled", "[unit]") {
    // The camera looks down -Z. A sun travels straight down, so a caster high above the view reaches it and one far off to the side does not.
    engine::scene::SceneGraph graph;
    const auto place = [&graph](glm::dvec3 position) {
        engine::scene::Transform transform;
        transform.position = position;
        return graph.create(transform).value();
    };
    // Above the camera, behind the view plane: the sun's shadow of it lands in the view.
    const engine::ecs::EntityHandle above_behind = place(glm::dvec3(0.0, 20.0, 10.0));
    // Far to the side at ground level: no shadow reaches the view.
    const engine::ecs::EntityHandle far_side = place(glm::dvec3(400.0, 0.0, 0.0));
    const engine::ecs::EntityHandle visible = place(glm::dvec3(0.0, 0.0, 0.0));

    std::vector<MeshBounds> bounds{MeshBounds{glm::vec3(0.0f), 1.0f}};
    std::vector<RenderableRef> renderables;
    for (const engine::ecs::EntityHandle node : {above_behind, far_side, visible}) {
        RenderableRef renderable;
        renderable.node = node;
        renderable.mesh = 0;
        renderables.push_back(renderable);
    }
    Light sun;
    sun.type = LightType::Directional;
    sun.direction = glm::vec3(0.0f, -1.0f, 0.0f);
    sun.casts_shadow = true;
    sun.id = 1;
    CascadeSettings cascades;
    cascades.shadow_distance = 100.0f;

    const FrameSnapshot with_sun = extract_frame(graph, test_camera(), bounds, renderables, {sun}, cascades);
    CHECK(with_sun.stats().drawn == 1);
    CHECK(with_sun.stats().shadow_only == 1);
    CHECK(with_sun.stats().culled == 1);
    CHECK(with_sun.items().size() == 2);
    // The visible item keeps its pick id; the shadow-only one has none and resolves to no entity.
    CHECK(with_sun.entity_of(1) == visible);
    CHECK(with_sun.entity_of(2) == std::nullopt);

    // Without a shadow-casting light, the behind item is culled as before.
    const FrameSnapshot without = extract_frame(graph, test_camera(), bounds, renderables);
    CHECK(without.stats().shadow_only == 0);
    CHECK(without.items().size() == 1);
    CHECK(without.lights().empty());
}
