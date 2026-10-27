// Skinning and motion vectors on the CPU (US-036). The skinned path is checked against the static path with transforms whose result is
// known: a rest-pose palette must reproduce the static mesh exactly, a palette that moves the vertices must match a static node moved the same
// way, and the motion vector of a moved surface must be the same whether the motion came from its palette or from its node.

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/reference_raster.hpp"

#include "engine/scene/scene_graph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace engine::render;
using engine::scene::Transform;

namespace {

using Catch::Matchers::WithinAbs;

// A unit box whose vertices are all weighted to joint `joint`.
MeshData rigid_skinned_box(std::uint32_t joint) {
    MeshData mesh = make_box_mesh(glm::vec3(0.5f));
    mesh.joints.assign(mesh.positions.size(), glm::uvec4(joint, 0, 0, 0));
    mesh.weights.assign(mesh.positions.size(), glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    return mesh;
}

// The golden camera from the scene fixture: it looks down -Z from z = 10.
Camera camera_at(glm::dvec3 position) {
    Camera camera;
    camera.position = position;
    camera.aspect = 1.0;
    return camera;
}

}  // namespace

TEST_CASE("Skinned mesh data is checked: one joint set and weight set per vertex, weights that sum to one, and joints in range", "[unit]") {
    CHECK_FALSE(validate_mesh(rigid_skinned_box(0)).has_value());
    CHECK(skin_joint_count(rigid_skinned_box(0)) == 1);
    CHECK(skin_joint_count(rigid_skinned_box(5)) == 6);
    CHECK(skin_joint_count(make_box_mesh(glm::vec3(1.0f))) == 0);

    MeshData short_list = rigid_skinned_box(0);
    short_list.weights.pop_back();
    CHECK(validate_mesh(short_list).has_value());

    MeshData unnormalized = rigid_skinned_box(0);
    unnormalized.weights[3] = glm::vec4(0.5f, 0.3f, 0.0f, 0.0f);
    CHECK(validate_mesh(unnormalized).has_value());

    MeshData too_many = rigid_skinned_box(0);
    too_many.joints[2] = glm::uvec4(kMaxSkinJoints, 0, 0, 0);
    CHECK(validate_mesh(too_many).has_value());
}

TEST_CASE("Extraction copies a skin pose into the snapshot, and records the previous pose and transform of a moved node", "[unit]") {
    engine::scene::SceneGraph graph;
    Transform transform;
    transform.position = glm::dvec3(0.0, 0.0, -5.0);
    const engine::ecs::EntityHandle node = graph.create(transform).value();
    Transform moved = transform;
    moved.position = glm::dvec3(0.0, 0.0, -4.0);
    static_cast<void>(graph.set_local(node, moved));

    const std::vector<MeshBounds> bounds{compute_bounds(rigid_skinned_box(0))};
    SkinPose pose;
    pose.palette = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f))};
    RenderableRef renderable;
    renderable.node = node;
    renderable.mesh = 0;
    renderable.skin = &pose;
    renderable.previous_world = glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, -5.0));

    const FrameSnapshot snapshot = extract_frame(graph, camera_at(glm::dvec3(0.0)), bounds, {renderable});
    REQUIRE(snapshot.items().size() == 1);
    const SnapshotItem& item = snapshot.items()[0];
    CHECK(item.bone_count == 2);
    CHECK(item.palette == 0);
    REQUIRE(snapshot.palettes().size() == 2);
    CHECK(snapshot.palettes()[1][3].y == 0.5f);
    // With no previous palette, the previous pose is the current one.
    CHECK(snapshot.previous_palettes()[1][3].y == 0.5f);
    // The node moved from z = -5 to z = -4, so its previous model is the current one shifted back by one meter.
    CHECK_THAT(item.model[3].z, WithinAbs(-4.0f, 1e-5f));
    CHECK_THAT(item.previous_model[3].z, WithinAbs(-5.0f, 1e-5f));
}

TEST_CASE("A rest-pose palette reproduces the static mesh pixel for pixel, with zero motion", "[unit]") {
    const std::vector<MeshData> meshes{make_box_mesh(glm::vec3(0.5f)), rigid_skinned_box(0)};
    const std::vector<MeshBounds> bounds{compute_bounds(meshes[0]), compute_bounds(meshes[1])};
    engine::scene::SceneGraph graph;
    Transform transform;
    transform.position = glm::dvec3(0.0, 0.0, -3.0);
    const engine::ecs::EntityHandle node = graph.create(transform).value();
    RenderableRef still;
    still.node = node;
    still.mesh = 0;
    const Camera camera = camera_at(glm::dvec3(0.0));
    const FrameSnapshot static_frame = extract_frame(graph, camera, bounds, {still});

    SkinPose rest;
    rest.palette = {glm::mat4(1.0f)};
    RenderableRef skinned = still;
    skinned.mesh = 1;
    skinned.skin = &rest;
    const FrameSnapshot skinned_frame = extract_frame(graph, camera, bounds, {skinned});

    const SceneImage expected = rasterize_reference(static_frame, meshes, 64, 64);
    const SceneImage actual = rasterize_reference(skinned_frame, meshes, 64, 64);
    CHECK(actual.pick_ids == expected.pick_ids);
    CHECK(actual.depth == expected.depth);
    CHECK(actual.color == expected.color);
    for (const glm::vec2& velocity : actual.velocity) {
        CHECK(velocity == glm::vec2(0.0f));
    }
}

TEST_CASE("A palette that moves the vertices draws them where a static node moved the same way would, and the motion vectors agree", "[unit]") {
    const std::vector<MeshData> meshes{make_box_mesh(glm::vec3(0.5f)), rigid_skinned_box(0)};
    const std::vector<MeshBounds> bounds{compute_bounds(meshes[0]), compute_bounds(meshes[1])};
    const Camera camera = camera_at(glm::dvec3(0.0));
    const glm::mat4 shift = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f));

    // A static box at x = 0.5 that was at x = 0 one frame ago, and a skinned box at the origin whose palette moved it from x = 0 to x = 0.5.
    engine::scene::SceneGraph graph;
    Transform now;
    now.position = glm::dvec3(0.5, 0.0, -3.0);
    const engine::ecs::EntityHandle node = graph.create(now).value();
    RenderableRef moved_node;
    moved_node.node = node;
    moved_node.mesh = 0;
    moved_node.previous_world = glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, -3.0));
    const FrameSnapshot node_frame = extract_frame(graph, camera, bounds, {moved_node});

    Transform origin;
    origin.position = glm::dvec3(0.0, 0.0, -3.0);
    engine::scene::SceneGraph skinned_graph;
    const engine::ecs::EntityHandle skinned_node = skinned_graph.create(origin).value();
    SkinPose pose;
    pose.palette = {shift};
    pose.previous = {glm::mat4(1.0f)};
    RenderableRef moved_skin;
    moved_skin.node = skinned_node;
    moved_skin.mesh = 1;
    moved_skin.skin = &pose;
    const FrameSnapshot skin_frame = extract_frame(skinned_graph, camera, bounds, {moved_skin});

    const SceneImage by_node = rasterize_reference(node_frame, meshes, 64, 64, MotionHistory{node_frame.view_projection(), camera.position});
    const SceneImage by_palette = rasterize_reference(skin_frame, meshes, 64, 64, MotionHistory{skin_frame.view_projection(), camera.position});
    CHECK(by_palette.pick_ids == by_node.pick_ids);
    CHECK(by_palette.depth == by_node.depth);

    // The box covers the center. It moved toward +x, so its motion is positive in x and zero in y.
    const std::size_t center = by_node.index(32, 32);
    REQUIRE(by_node.pick_ids[center] != 0);
    CHECK(by_node.velocity[center].x > 0.0f);
    CHECK_THAT(by_node.velocity[center].y, WithinAbs(0.0f, 1e-6f));
    for (std::size_t i = 0; i < by_node.velocity.size(); ++i) {
        INFO("pixel " << i);
        CHECK(std::abs(by_palette.velocity[i].x - by_node.velocity[i].x) < 1e-5f);
        CHECK(std::abs(by_palette.velocity[i].y - by_node.velocity[i].y) < 1e-5f);
    }
}

TEST_CASE("Camera motion alone produces motion vectors on a still object, pointing the way the scene slides on screen", "[unit]") {
    const std::vector<MeshData> meshes{make_box_mesh(glm::vec3(0.5f))};
    const std::vector<MeshBounds> bounds{compute_bounds(meshes[0])};
    engine::scene::SceneGraph graph;
    Transform transform;
    transform.position = glm::dvec3(0.0, 0.0, -3.0);
    const engine::ecs::EntityHandle node = graph.create(transform).value();
    RenderableRef still;
    still.node = node;
    still.mesh = 0;

    // The camera moves right by 0.2 m, so the box slides left on screen: its motion in UV units is negative in x.
    const FrameSnapshot before = extract_frame(graph, camera_at(glm::dvec3(0.0)), bounds, {still});
    const FrameSnapshot after = extract_frame(graph, camera_at(glm::dvec3(0.2, 0.0, 0.0)), bounds, {still});
    const SceneImage image = rasterize_reference(after, meshes, 64, 64, MotionHistory{before.view_projection(), before.camera().position});
    const std::size_t center = image.index(32, 32);
    REQUIRE(image.pick_ids[center] != 0);
    CHECK(image.velocity[center].x < 0.0f);
    CHECK_THAT(image.velocity[center].y, WithinAbs(0.0f, 1e-6f));
}
