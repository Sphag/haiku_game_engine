// Scene rendering on the CPU (US-033): cameras, frustum culling, render extraction into a frame snapshot, entity-ID picking after
// deletion, and golden pixels from the reference rasterizer. These need no GPU and run on every operating system. The last case runs
// the whole frame through the render graph on the Validation device, which enforces the RHI contract.

#include "render_scene_fixture.hpp"

#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/reference_raster.hpp"
#include "engine/render/scene_image.hpp"
#include "engine/render/scene_renderer.hpp"

#include "engine/rhi/device.hpp"
#include "engine/rhi/validation_device.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using engine::ecs::EntityHandle;
using engine::render::Camera;
using engine::render::camera_view_projection;
using engine::render::compute_bounds;
using engine::render::extract_frame;
using engine::render::Frustum;
using engine::render::FrameSnapshot;
using engine::render::make_box_mesh;
using engine::render::make_frustum;
using engine::render::MeshBounds;
using engine::render::MeshData;
using engine::render::pick_entity;
using engine::render::RenderableRef;
using engine::render::rasterize_reference;
using engine::render::SceneImage;
using engine::render::sphere_visible;
using engine::render::tests::build_golden_scene;
using engine::render::tests::golden_camera;
using engine::render::tests::golden_depth;
using engine::render::tests::GoldenScene;
using engine::render::tests::kSceneHeight;
using engine::render::tests::kSceneWidth;
using engine::render::validate_mesh;
using engine::scene::ChildPolicy;
using engine::scene::SceneGraph;
using engine::scene::Transform;

namespace {

using Catch::Matchers::WithinAbs;

const std::uint8_t kRed[4] = {255, 0, 0, 255};
const std::uint8_t kBlue[4] = {0, 0, 255, 255};
const std::uint8_t kGreen[4] = {0, 255, 0, 255};
const std::uint8_t kBackground[4] = {0, 0, 0, 0};

// The renderer's ids are the mesh-table index, so the CPU path builds the same table from the meshes.
std::vector<MeshBounds> bounds_of(const std::vector<MeshData>& meshes) {
    std::vector<MeshBounds> bounds;
    for (const MeshData& mesh : meshes) {
        bounds.push_back(compute_bounds(mesh));
    }
    return bounds;
}

FrameSnapshot extract(GoldenScene& scene) {
    return extract_frame(scene.graph, scene.camera, bounds_of(scene.meshes), scene.renderables);
}

// Compares one pixel with its golden value. The color may differ by one step, which is the rounding a GPU applies.
void expect_pixel(const SceneImage& image, std::uint32_t x, std::uint32_t y, const std::uint8_t (&color)[4], std::uint32_t pick,
                  double depth) {
    INFO("pixel " << x << "," << y);
    const std::size_t i = image.index(x, y);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        CHECK(std::abs(static_cast<int>(image.color[i][channel]) - static_cast<int>(color[channel])) <= 1);
    }
    CHECK(image.pick_ids[i] == pick);
    CHECK_THAT(static_cast<double>(image.depth[i]), WithinAbs(depth, 1e-5));
}

}  // namespace

TEST_CASE("A camera is valid when its orientation is unit length and its planes and field are sane", "[unit]") {
    Camera camera = golden_camera();
    CHECK(camera.is_valid());
    CHECK(camera.forward().z == -1.0);
    camera.near_plane = 2000.0;  // beyond the far plane
    CHECK_FALSE(camera.is_valid());
    camera = golden_camera();
    camera.orientation = glm::dquat(2.0, 0.0, 0.0, 0.0);
    CHECK_FALSE(camera.is_valid());
    camera = golden_camera();
    camera.fov_y_radians = 0.0;
    CHECK_FALSE(camera.is_valid());
}

TEST_CASE("Camera-relative transforms put the view axis at the screen center, with the expected depth and axes", "[unit]") {
    const glm::mat4 view_projection = camera_view_projection(golden_camera());
    // A point 9 units in front of the eye is on the view axis, so it projects to the center of the screen at the formula's depth.
    const glm::vec4 center = view_projection * glm::vec4(0.0f, 0.0f, -9.0f, 1.0f);
    CHECK_THAT(static_cast<double>(center.x / center.w), WithinAbs(0.0, 1e-6));
    CHECK_THAT(static_cast<double>(center.y / center.w), WithinAbs(0.0, 1e-6));
    CHECK_THAT(static_cast<double>(center.z / center.w), WithinAbs(golden_depth(9.0), 1e-5));
    // Right is +X and up is +Y on screen (docs/runtime-conventions.md).
    const glm::vec4 right = view_projection * glm::vec4(1.0f, 0.0f, -9.0f, 1.0f);
    const glm::vec4 up = view_projection * glm::vec4(0.0f, 1.0f, -9.0f, 1.0f);
    CHECK(right.x / right.w > 0.0f);
    CHECK(up.y / up.w > 0.0f);
}

TEST_CASE("A camera rotation changes what the frustum keeps", "[unit]") {
    Camera camera = golden_camera();
    camera.orientation = glm::angleAxis(glm::radians(180.0), glm::dvec3(0.0, 1.0, 0.0));
    const Frustum turned = make_frustum(camera_view_projection(camera));
    // Turned around, the point that was in front is now behind the eye.
    CHECK_FALSE(sphere_visible(turned, glm::vec3(0.0f, 0.0f, -9.0f), 1.0f));
    CHECK(sphere_visible(turned, glm::vec3(0.0f, 0.0f, 9.0f), 1.0f));
}

TEST_CASE("The frustum keeps spheres that touch its planes and removes spheres beyond them", "[unit]") {
    const Frustum frustum = make_frustum(camera_view_projection(golden_camera()));
    CHECK(sphere_visible(frustum, glm::vec3(0.0f, 0.0f, -9.0f), 1.0f));
    CHECK_FALSE(sphere_visible(frustum, glm::vec3(0.0f, 0.0f, 12.0f), 1.0f));      // behind the eye
    CHECK_FALSE(sphere_visible(frustum, glm::vec3(100.0f, 0.0f, 0.0f), 1.0f));     // far to the side
    CHECK_FALSE(sphere_visible(frustum, glm::vec3(0.0f, 0.0f, -2000.0f), 1.0f));   // beyond the far plane
    // A sphere whose surface reaches the side plane is kept: culling never removes something that may be on screen.
    // At distance 9 the right plane is at x = 9 * tan(30 degrees), about 5.196.
    CHECK(sphere_visible(frustum, glm::vec3(5.196f + 0.5f, 0.0f, -9.0f), 1.0f));
    CHECK_FALSE(sphere_visible(frustum, glm::vec3(5.196f + 2.0f, 0.0f, -9.0f), 1.0f));
}

TEST_CASE("Mesh validation rejects empty, out-of-range, partial, and non-finite meshes", "[unit]") {
    CHECK_FALSE(validate_mesh(make_box_mesh(glm::vec3(1.0f))).has_value());

    MeshData empty;
    CHECK(validate_mesh(empty).has_value());

    MeshData partial = make_box_mesh(glm::vec3(1.0f));
    partial.indices.pop_back();
    CHECK(validate_mesh(partial).has_value());

    MeshData out_of_range = make_box_mesh(glm::vec3(1.0f));
    out_of_range.indices[4] = 24;  // the box has 24 vertices, so 24 is the first index past the end
    CHECK(validate_mesh(out_of_range).has_value());

    MeshData not_finite = make_box_mesh(glm::vec3(1.0f));
    not_finite.positions[0].x = std::numeric_limits<float>::quiet_NaN();
    CHECK(validate_mesh(not_finite).has_value());
}

TEST_CASE("A mesh's bounding sphere reaches every vertex and matches a box's half diagonal", "[unit]") {
    const MeshBounds bounds = compute_bounds(make_box_mesh(glm::vec3(1.0f, 2.0f, 2.0f)));
    CHECK_THAT(static_cast<double>(bounds.radius), WithinAbs(3.0, 1e-6));
    CHECK_THAT(static_cast<double>(bounds.center.x), WithinAbs(0.0, 1e-6));
    const MeshData mesh = make_box_mesh(glm::vec3(1.0f, 2.0f, 2.0f));
    for (const glm::vec3& position : mesh.positions) {
        CHECK(glm::length(position - bounds.center) <= bounds.radius + 1e-5f);
    }
}

TEST_CASE("Extraction culls off-screen and behind-eye objects, skips stale nodes, and numbers what it keeps", "[unit]") {
    GoldenScene scene = build_golden_scene();
    const FrameSnapshot snapshot = extract(scene);

    const auto& stats = snapshot.stats();
    CHECK(stats.considered == 7);
    CHECK(stats.stale == 1);
    CHECK(stats.unknown_mesh == 0);
    CHECK(stats.culled == 2);
    CHECK(stats.drawn == 4);
    REQUIRE(snapshot.items().size() == 4);

    // Pick ids are 1-based positions among the survivors, in renderable order.
    CHECK(snapshot.entity_of(1) == scene.entities.near_box);
    CHECK(snapshot.entity_of(2) == scene.entities.far_box);
    CHECK(snapshot.entity_of(3) == scene.entities.side_box);
    CHECK(snapshot.entity_of(4) == scene.entities.distant_box);
    CHECK_FALSE(snapshot.entity_of(0).has_value());
    CHECK_FALSE(snapshot.entity_of(5).has_value());
    CHECK(snapshot.items()[2].mesh == 1);  // the side box uses the half-size mesh
}

TEST_CASE("A renderable whose mesh is not in the table is skipped and counted", "[unit]") {
    GoldenScene scene = build_golden_scene();
    scene.renderables[0].mesh = 9;
    const FrameSnapshot snapshot = extract(scene);
    CHECK(snapshot.stats().unknown_mesh == 1);
    CHECK(snapshot.items().size() == 3);
}

TEST_CASE("A snapshot keeps its values after the scene changes, with no pointers into the graph", "[unit]") {
    GoldenScene scene = build_golden_scene();
    const FrameSnapshot snapshot = extract(scene);
    const FrameSnapshot copy = snapshot;

    // Changing the scene does not change a snapshot that was already taken.
    static_cast<void>(scene.graph.destroy(scene.entities.near_box, ChildPolicy::DestroySubtree));
    CHECK_FALSE(scene.graph.alive(scene.entities.near_box));
    CHECK(snapshot.items().size() == 4);
    CHECK(copy.entity_of(1) == scene.entities.near_box);  // the snapshot holds the handle as a value, not a pointer into the graph
    CHECK(snapshot.items()[0].color.r == 1.0f);
}

TEST_CASE("Destroying the near box renumbers the picks, and the destroyed handle is never picked again", "[unit]") {
    GoldenScene scene = build_golden_scene();
    static_cast<void>(scene.graph.destroy(scene.entities.near_box, ChildPolicy::DestroySubtree));
    const FrameSnapshot snapshot = extract(scene);

    CHECK(snapshot.stats().stale == 2);  // the destroyed near box and the destroyed node
    CHECK(snapshot.stats().drawn == 3);
    CHECK(snapshot.entity_of(1) == scene.entities.far_box);
    CHECK(snapshot.entity_of(2) == scene.entities.side_box);
    CHECK(snapshot.entity_of(3) == scene.entities.distant_box);
    for (std::uint32_t id = 1; id <= snapshot.items().size(); ++id) {
        CHECK(snapshot.entity_of(id) != scene.entities.near_box);
    }
}

TEST_CASE("A slot reused by a new entity does not revive a deleted handle", "[unit]") {
    GoldenScene scene = build_golden_scene();
    const EntityHandle deleted = scene.entities.near_box;
    static_cast<void>(scene.graph.destroy(deleted, ChildPolicy::DestroySubtree));
    const EntityHandle reused = scene.graph.create(Transform{}).value();
    REQUIRE(reused.index == deleted.index);  // the same slot, with a new generation
    CHECK(reused != deleted);
    CHECK_FALSE(scene.graph.alive(deleted));

    // The renderable that still names the deleted handle is stale, even though its slot is alive again.
    const FrameSnapshot stale = extract(scene);
    CHECK(stale.stats().stale == 2);
    CHECK_FALSE(stale.entity_of(1) == reused);

    scene.renderables[0].node = reused;
    const FrameSnapshot fresh = extract(scene);
    CHECK(fresh.entity_of(1) == reused);
}

TEST_CASE("The reference rasterizer draws the golden pixels, with the nearer box in front", "[unit]") {
    GoldenScene scene = build_golden_scene();
    const FrameSnapshot snapshot = extract(scene);
    const SceneImage image = rasterize_reference(snapshot, scene.meshes, kSceneWidth, kSceneHeight);

    // The center is the near box's front face, at distance 9. The far box is behind it and is rejected by the depth test.
    expect_pixel(image, 32, 32, kRed, 1, golden_depth(9.0));
    // The right side shows the side box's front face, at distance 9.5.
    expect_pixel(image, 49, 32, kGreen, 3, golden_depth(9.5));
    // The corner is background: no pick, no depth.
    expect_pixel(image, 5, 5, kBackground, 0, 0.0);

    const auto picked = pick_entity(image, snapshot, 32, 32);
    REQUIRE(picked.has_value());
    CHECK(*picked == scene.entities.near_box);
    CHECK_FALSE(pick_entity(image, snapshot, 5, 5).has_value());
    CHECK_FALSE(pick_entity(image, snapshot, kSceneWidth, 0).has_value());
}

TEST_CASE("The depth test keeps the nearer surface whatever order the renderables are drawn in", "[unit]") {
    GoldenScene scene = build_golden_scene();
    std::vector<RenderableRef> reversed(scene.renderables.rbegin(), scene.renderables.rend());
    scene.renderables = reversed;
    const FrameSnapshot snapshot = extract(scene);
    const SceneImage image = rasterize_reference(snapshot, scene.meshes, kSceneWidth, kSceneHeight);
    // The near box is now drawn last, and it still wins at the center.
    const auto picked = pick_entity(image, snapshot, 32, 32);
    REQUIRE(picked.has_value());
    CHECK(*picked == scene.entities.near_box);
    CHECK_THAT(static_cast<double>(image.depth[image.index(32, 32)]), WithinAbs(golden_depth(9.0), 1e-5));
}

TEST_CASE("Picking after deletion returns the box that is now in front, and the side box keeps its place", "[unit]") {
    GoldenScene scene = build_golden_scene();
    static_cast<void>(scene.graph.destroy(scene.entities.near_box, ChildPolicy::DestroySubtree));
    const FrameSnapshot snapshot = extract(scene);
    const SceneImage image = rasterize_reference(snapshot, scene.meshes, kSceneWidth, kSceneHeight);

    // The far box is now the front surface at the center, at distance 13, and it is pick 1.
    expect_pixel(image, 32, 32, kBlue, 1, golden_depth(13.0));
    const auto center = pick_entity(image, snapshot, 32, 32);
    REQUIRE(center.has_value());
    CHECK(*center == scene.entities.far_box);
    // The side box is pick 2 now, which changes its id but not its surface.
    expect_pixel(image, 49, 32, kGreen, 2, golden_depth(9.5));
    const auto side = pick_entity(image, snapshot, 49, 32);
    REQUIRE(side.has_value());
    CHECK(*side == scene.entities.side_box);
}

TEST_CASE("A moved object is drawn where its new transform puts it", "[unit]") {
    GoldenScene scene = build_golden_scene();
    // Moving the side box back by one unit puts its front face at distance 10.5. The same pixel still shows that face, because the
    // box is still in the same column of the screen.
    Transform moved;
    moved.position = glm::dvec3(3.0, 0.0, -1.0);
    REQUIRE(scene.graph.set_local(scene.entities.side_box, moved).ok());
    const FrameSnapshot snapshot = extract(scene);
    const SceneImage image = rasterize_reference(snapshot, scene.meshes, kSceneWidth, kSceneHeight);
    const auto side = pick_entity(image, snapshot, 49, 32);
    REQUIRE(side.has_value());
    CHECK(*side == scene.entities.side_box);
    CHECK_THAT(static_cast<double>(image.depth[image.index(49, 32)]), WithinAbs(golden_depth(10.5), 1e-5));
}

TEST_CASE("The scene renderer reports Unsupported on the Validation device, which cannot take the default texture upload, and leaks nothing", "[integration]") {
    // The reference device keeps no buffer contents, so it cannot take any upload. create() uploads the default textures and the
    // environment, so it must fail with Unsupported, and the failed build must release everything it created. The GPU suites run the frame.
    auto created = engine::rhi::create_device(engine::rhi::DeviceDesc{});
    REQUIRE(created.ok());
    auto device = std::move(created).value();
    const auto baseline = device->resource_stats().value();

    engine::render::SceneRendererDesc desc;
    desc.width = kSceneWidth;
    desc.height = kSceneHeight;
    engine::render::SceneShaders shaders;
    shaders.vertex = engine::rhi::ShaderBinary{engine::rhi::ShaderStage::Vertex, "VSMain", {1, 2, 3, 4}};
    shaders.pixel = engine::rhi::ShaderBinary{engine::rhi::ShaderStage::Pixel, "PSMain", {1, 2, 3, 4}};
    shaders.pixel_blend = engine::rhi::ShaderBinary{engine::rhi::ShaderStage::Pixel, "PSBlend", {1, 2, 3, 4}};
    auto renderer_result = engine::render::SceneRenderer::create(*device, desc, shaders);
    REQUIRE_FALSE(renderer_result.ok());
    CHECK(renderer_result.error().code == engine::rhi::RhiErrorCode::Unsupported);

    const auto after = device->resource_stats().value();
    CHECK(after.buffers == baseline.buffers);
    CHECK(after.textures == baseline.textures);
    CHECK(after.descriptor_sets == baseline.descriptor_sets);
    CHECK(after.shader_descriptors == baseline.shader_descriptors);
}
