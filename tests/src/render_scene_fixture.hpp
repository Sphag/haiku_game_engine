#pragma once

// Golden scene for the scene renderer (US-033). The camera looks down -Z from z = 10 with a 60 degree vertical field, and a 64 x 64
// target. The scene covers every rule the story names: nearer and farther boxes that overlap on screen (depth), an object off to one
// side and one behind the camera (culling), a renderable whose node was destroyed (stale), and a box far behind the near box (depth again).
// The expected pixels are worked out from the geometry, not read from the renderer. The depth values come from the perspective formula
// for depth in [0, 1] with near = 0.1 and far = 1000.

#include "engine/render/camera.hpp"
#include "engine/render/frame_snapshot.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/scene_image.hpp"

#include "engine/scene/scene_graph.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace engine::render::tests {

inline constexpr std::uint32_t kSceneWidth = 64;
inline constexpr std::uint32_t kSceneHeight = 64;

// The entities of the golden scene, in renderable order (the stale renderable is last).
struct GoldenEntities {
    ecs::EntityHandle near_box;    // A: red, at the origin, in front
    ecs::EntityHandle far_box;     // B: blue, 4 units behind A, hidden by A at the center
    ecs::EntityHandle side_box;    // C: green, to the right, half size
    ecs::EntityHandle distant_box; // D: yellow, 300 units behind A
    ecs::EntityHandle off_screen;  // E: white, far to the right: culled
    ecs::EntityHandle behind_eye;  // F: white, behind the camera: culled
    ecs::EntityHandle destroyed;   // G: its node was destroyed before extraction: stale
};

struct GoldenScene {
    scene::SceneGraph graph;
    std::vector<MeshData> meshes;  // 0: box with half extent 1, 1: box with half extent 0.5
    std::vector<RenderableRef> renderables;
    GoldenEntities entities;
    Camera camera;
};

inline Camera golden_camera() {
    Camera camera;
    camera.position = glm::dvec3(0.0, 0.0, 10.0);
    camera.fov_y_radians = 1.0471975511965976;  // 60 degrees
    camera.aspect = 1.0;
    camera.near_plane = 0.1;
    camera.far_plane = 1000.0;
    return camera;
}

inline GoldenScene build_golden_scene() {
    GoldenScene scene;
    scene.camera = golden_camera();
    scene.meshes = {make_box_mesh(glm::vec3(1.0f)), make_box_mesh(glm::vec3(0.5f))};

    const auto place = [&scene](glm::dvec3 position) {
        scene::Transform transform;
        transform.position = position;
        return scene.graph.create(transform).value();
    };
    scene.entities.near_box = place(glm::dvec3(0.0, 0.0, 0.0));
    scene.entities.far_box = place(glm::dvec3(0.0, 0.0, -4.0));
    scene.entities.side_box = place(glm::dvec3(3.0, 0.0, 0.0));
    scene.entities.distant_box = place(glm::dvec3(0.0, 0.0, -300.0));
    scene.entities.off_screen = place(glm::dvec3(100.0, 0.0, 0.0));
    scene.entities.behind_eye = place(glm::dvec3(0.0, 0.0, 20.0));
    scene.entities.destroyed = place(glm::dvec3(0.0, 2.0, 0.0));
    static_cast<void>(scene.graph.destroy(scene.entities.destroyed, scene::ChildPolicy::DestroySubtree));

    const auto add = [&scene](ecs::EntityHandle node, std::uint32_t mesh, glm::vec4 color) {
        RenderableRef renderable;
        renderable.node = node;
        renderable.mesh = mesh;
        renderable.color = color;
        scene.renderables.push_back(renderable);
    };
    add(scene.entities.near_box, 0, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    add(scene.entities.far_box, 0, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    add(scene.entities.side_box, 1, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    add(scene.entities.distant_box, 0, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
    add(scene.entities.off_screen, 0, glm::vec4(1.0f));
    add(scene.entities.behind_eye, 0, glm::vec4(1.0f));
    add(scene.entities.destroyed, 0, glm::vec4(1.0f));
    return scene;
}

// NDC depth of a point at `distance` in front of the camera, for the perspective with near 0.1 and far 1000 (depth in [0, 1]).
inline double golden_depth(double distance) {
    const double near = 0.1;
    const double far = 1000.0;
    return far / (far - near) - far * near / ((far - near) * distance);
}

}  // namespace engine::render::tests
