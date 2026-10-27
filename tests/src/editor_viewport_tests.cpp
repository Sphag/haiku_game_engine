// The scene viewport (US-063): the editor camera, projection and picking, the gizmo handles, drags in each mode and space,
// snapping, DPI scaling, a resized surface, and the selection after removal and reparenting. No window or graphics device is used.

#include "engine/core/guid.hpp"
#include "engine/editor/console.hpp"
#include "engine/editor/scene_editor.hpp"
#include "engine/editor/viewport.hpp"
#include "engine/scene/transform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <memory>
#include <optional>

using engine::core::Guid;
using engine::editor::Console;
using engine::editor::EditorCamera;
using engine::editor::EditorViewport;
using engine::editor::GizmoMode;
using engine::editor::GizmoSpace;
using engine::editor::ViewportInput;
using engine::editor::ViewportSurface;
using engine::editor::SceneEditor;
using engine::scene::Transform;

namespace {

constexpr double kTolerance = 1e-6;

bool near(double a, double b, double tolerance = kTolerance) {
    return std::abs(a - b) <= tolerance;
}

bool near(const glm::dvec3& a, const glm::dvec3& b, double tolerance = kTolerance) {
    return glm::length(a - b) <= tolerance;
}

// A scene with a viewport, a 1000 x 800 surface at DPI 1, and helpers that press, move, and release the pointer.
struct Harness {
    std::shared_ptr<Console> console = std::make_shared<Console>();
    SceneEditor scene{console};
    EditorViewport viewport;
    ViewportSurface surface{1000.0, 800.0, 1.0};

    Guid add(const glm::dvec3& position, const Guid& parent = {}) {
        auto created = scene.create_entity(parent);
        REQUIRE(created.ok());
        Transform local;
        local.position = position;
        REQUIRE(scene.set_transform(created.value(), local).ok());
        return created.value();
    }

    glm::dvec3 world(const Guid& entity) {
        return glm::dvec3((*scene.world_matrix(entity))[3]);
    }

    Transform local(const Guid& entity) const {
        return *scene.scene().transform(entity);
    }

    glm::dvec2 pixel_of(const glm::dvec3& point) const {
        const auto px = engine::editor::project_point(viewport.camera().view_projection(surface), surface, point);
        REQUIRE(px.has_value());
        return *px;
    }

    ViewportInput input(glm::dvec2 pixel) const {
        ViewportInput in;
        in.surface = surface;
        in.hovered = true;
        in.pointer = pixel;
        return in;
    }

    void frame(ViewportInput in) {
        viewport.update(scene, in);
    }

    void press(glm::dvec2 pixel) {
        ViewportInput in = input(pixel);
        in.primary_pressed = true;
        in.primary_down = true;
        frame(in);
    }

    void move_to(glm::dvec2 pixel) {
        ViewportInput in = input(pixel);
        in.primary_down = true;
        frame(in);
    }

    void release_at(glm::dvec2 pixel) {
        ViewportInput in = input(pixel);
        in.primary_down = false;
        frame(in);
    }

    // Selects an entity and builds its gizmo with no press, so the handle positions are known before the drag.
    double gizmo_length_of(const Guid& entity) {
        scene.select(entity);
        frame(input(pixel_of(world(entity))));
        REQUIRE(viewport.gizmo().has_value());
        return viewport.gizmo()->length;
    }
};

}  // namespace

TEST_CASE("The editor camera projects a point to the pixel whose ray passes through it", "[unit][editor]") {
    Harness h;
    h.viewport.camera().target = {1.0, 2.0, 3.0};
    h.viewport.camera().yaw = 0.3;
    h.viewport.camera().pitch = 0.7;
    const auto vp = h.viewport.camera().view_projection(h.surface);
    for (const glm::dvec3& point : {glm::dvec3{1.0, 2.0, 3.0}, glm::dvec3{4.5, -1.0, 0.25}, glm::dvec3{-7.0, 3.0, 9.0}}) {
        const auto px = engine::editor::project_point(vp, h.surface, point);
        REQUIRE(px.has_value());
        const engine::editor::Ray ray = engine::editor::camera_ray(vp, h.surface, *px);
        CHECK(glm::length(glm::cross(point - ray.origin, ray.direction)) < 1e-6);
        CHECK(glm::dot(point - ray.origin, ray.direction) > 0.0);
    }
    // The target is the centre of the view.
    const auto centre = engine::editor::project_point(vp, h.surface, {1.0, 2.0, 3.0});
    REQUIRE(centre.has_value());
    CHECK(near(centre->x, 500.0));
    CHECK(near(centre->y, 400.0));
}

TEST_CASE("Orbit keeps the distance, pan keeps the view direction, and zoom and frame move the camera", "[unit][editor]") {
    EditorCamera camera;
    const ViewportSurface surface{1000.0, 800.0, 1.0};
    const double distance = camera.distance;
    camera.orbit(120.0, -40.0);
    CHECK(near(camera.distance, distance));
    const glm::dvec3 forward = camera.forward();
    camera.pan(30.0, 12.0, surface);
    CHECK(near(glm::dot(camera.forward(), forward), 1.0, 1e-9));
    camera.dolly(2.0);
    CHECK(camera.distance < distance);
    camera.frame({5.0, 0.0, -2.0});
    CHECK(near(camera.target, glm::dvec3{5.0, 0.0, -2.0}));
}

TEST_CASE("A press on a marker selects that entity, and a press on empty space clears the selection", "[unit][editor]") {
    Harness h;
    h.add({0.0, 0.0, 0.0});
    const Guid marker = h.add({3.0, 0.0, 0.0});
    h.scene.select(std::nullopt);

    h.press(h.pixel_of({3.0, 0.0, 0.0}));
    CHECK(h.scene.selected() == marker);
    h.release_at(h.pixel_of({3.0, 0.0, 0.0}));

    h.scene.select(std::nullopt);
    h.press({5.0, 5.0});  // far from both markers
    CHECK(!h.scene.selected().has_value());
}

TEST_CASE("The pick radius and the gizmo scale with the DPI scale", "[unit][editor]") {
    Harness h;
    const Guid marker = h.add({3.0, 0.0, 0.0});
    const glm::dvec2 at = h.pixel_of({3.0, 0.0, 0.0});

    // Twelve pixels from the marker: outside the 8-pixel radius at DPI 1, inside the 16-pixel radius at DPI 2.
    h.scene.select(std::nullopt);
    h.press(at + glm::dvec2(12.0, 0.0));
    CHECK(!h.scene.selected().has_value());

    h.surface.dpi_scale = 2.0;
    h.press(at + glm::dvec2(12.0, 0.0));
    CHECK(h.scene.selected() == marker);

    // The gizmo is 1.5 times longer at DPI 1.5 on the same surface and camera.
    h.surface.dpi_scale = 1.0;
    const double at_one = h.gizmo_length_of(marker);
    h.surface.dpi_scale = 1.5;
    const double at_one_and_a_half = h.gizmo_length_of(marker);
    CHECK(near(at_one_and_a_half / at_one, 1.5, 1e-9));
}

TEST_CASE("A resized surface keeps the target at the centre of the view", "[unit][editor]") {
    Harness h;
    h.viewport.camera().target = {0.0, 0.0, 0.0};
    h.surface = ViewportSurface{500.0, 800.0, 1.0};
    const glm::dvec2 centre = h.pixel_of({0.0, 0.0, 0.0});
    CHECK(near(centre.x, 250.0));
    CHECK(near(centre.y, 400.0));
    // A collapsed surface draws nothing and takes no input.
    h.surface = ViewportSurface{0.0, 0.0, 1.0};
    h.scene.select(std::nullopt);
    h.press({0.0, 0.0});
    CHECK(!h.scene.selected().has_value());
}

TEST_CASE("A world-space move handle drags the entity along its axis by the distance the pointer moved", "[unit][editor]") {
    Harness h;
    const Guid entity = h.add({0.0, 0.0, 0.0});
    const double length = h.gizmo_length_of(entity);
    const glm::dvec3 x{1.0, 0.0, 0.0};

    h.press(h.pixel_of(x * (0.5 * length)));
    REQUIRE(h.viewport.dragging());
    h.move_to(h.pixel_of(x * (0.5 * length + 2.0)));
    CHECK(near(h.local(entity).position.x, 2.0));
    h.release_at(h.pixel_of(x * (0.5 * length + 2.0)));
    CHECK(!h.viewport.dragging());
}

TEST_CASE("Snapping rounds a move to the translate step", "[unit][editor]") {
    Harness h;
    const Guid entity = h.add({0.0, 0.0, 0.0});
    const double length = h.gizmo_length_of(entity);
    h.viewport.snap().enabled = true;
    h.viewport.snap().translate_step = 0.5;
    const glm::dvec3 x{1.0, 0.0, 0.0};

    h.press(h.pixel_of(x * (0.5 * length)));
    h.move_to(h.pixel_of(x * (0.5 * length + 1.3)));  // 2.6 steps rounds to 3, or 1.5 metres
    CHECK(near(h.local(entity).position.x, 1.5, 1e-6));
}

TEST_CASE("A local-space move follows the entity's rotation", "[unit][editor]") {
    Harness h;
    const Guid entity = h.add({0.0, 0.0, 0.0});
    Transform turned = h.local(entity);
    turned.rotation = glm::angleAxis(glm::radians(90.0), glm::dvec3{0.0, 1.0, 0.0});  // local X points to world -Z
    REQUIRE(h.scene.set_transform(entity, turned).ok());
    h.viewport.set_space(GizmoSpace::Local);
    const double length = h.gizmo_length_of(entity);

    const glm::dvec3 local_x_in_world{0.0, 0.0, -1.0};
    h.press(h.pixel_of(local_x_in_world * (0.5 * length)));
    REQUIRE(h.viewport.dragging());
    h.move_to(h.pixel_of(local_x_in_world * (0.5 * length + 2.0)));
    CHECK(near(h.world(entity), glm::dvec3{0.0, 0.0, -2.0}, 1e-6));
}

TEST_CASE("A child's move writes its local position through the parent's inverse transform", "[unit][editor]") {
    Harness h;
    const Guid parent = h.add({5.0, 0.0, 0.0});
    const Guid child = h.add({2.0, 0.0, 0.0}, parent);  // local 2 under a parent at 5: world 7
    REQUIRE(h.local(child).position.x == 2.0);
    const double length = h.gizmo_length_of(child);
    const glm::dvec3 x{1.0, 0.0, 0.0};
    const glm::dvec3 pivot{7.0, 0.0, 0.0};

    h.press(h.pixel_of(pivot + x * (0.5 * length)));
    REQUIRE(h.viewport.dragging());
    h.move_to(h.pixel_of(pivot + x * (0.5 * length + 1.0)));
    CHECK(near(h.local(child).position.x, 3.0, 1e-6));
    CHECK(near(h.world(child).x, 8.0, 1e-6));
}

TEST_CASE("A rotate handle turns the entity about its axis, snapped to the angle step", "[unit][editor]") {
    Harness h;
    const Guid entity = h.add({0.0, 0.0, 0.0});
    const double length = h.gizmo_length_of(entity);
    h.viewport.set_mode(GizmoMode::Rotate);
    h.viewport.snap().enabled = true;
    h.viewport.snap().rotate_step_degrees = 15.0;
    // Points on the Z ring at 45 and 80 degrees: their x and y are both non-zero, so the press is on the Z ring alone.
    const glm::dvec3 start{length * std::cos(glm::radians(45.0)), length * std::sin(glm::radians(45.0)), 0.0};
    const glm::dvec3 end{length * std::cos(glm::radians(80.0)), length * std::sin(glm::radians(80.0)), 0.0};

    h.press(h.pixel_of(start));
    REQUIRE(h.viewport.dragging());
    h.move_to(h.pixel_of(end));  // 35 degrees rounds to 30
    const glm::dquat expected = glm::angleAxis(glm::radians(30.0), glm::dvec3{0.0, 0.0, 1.0});
    const glm::dquat actual = h.local(entity).rotation;
    CHECK(near(std::abs(glm::dot(actual, expected)), 1.0, 1e-9));
}

TEST_CASE("A scale handle grows the entity along its own axis and never reaches zero", "[unit][editor]") {
    Harness h;
    const Guid entity = h.add({0.0, 0.0, 0.0});
    const double length = h.gizmo_length_of(entity);
    h.viewport.set_mode(GizmoMode::Scale);
    const glm::dvec3 x{1.0, 0.0, 0.0};

    h.press(h.pixel_of(x * (0.5 * length)));
    REQUIRE(h.viewport.dragging());
    h.move_to(h.pixel_of(x * (1.5 * length)));  // one gizmo length outward doubles the scale
    CHECK(near(h.local(entity).scale.x, 2.0, 1e-4));
    CHECK(near(h.local(entity).scale.y, 1.0));

    h.move_to(h.pixel_of(x * (-2.5 * length)));  // far past the origin: the factor is clamped, not zero or negative
    CHECK(h.local(entity).scale.x > 0.0);
}

TEST_CASE("A gizmo drag does not move the editor camera", "[unit][editor]") {
    Harness h;
    const Guid entity = h.add({0.0, 0.0, 0.0});
    const double length = h.gizmo_length_of(entity);
    const glm::dvec3 target = h.viewport.camera().target;
    const double yaw = h.viewport.camera().yaw;
    const double distance = h.viewport.camera().distance;

    h.press(h.pixel_of(glm::dvec3{1.0, 0.0, 0.0} * (0.5 * length)));
    REQUIRE(h.viewport.dragging());
    ViewportInput in = h.input(h.pixel_of(glm::dvec3{1.0, 0.0, 0.0} * (0.5 * length + 1.0)));
    in.primary_down = true;
    in.orbit_down = true;
    in.pan_down = true;
    in.wheel = 3.0;
    in.pointer_delta = {200.0, 90.0};
    h.frame(in);

    CHECK(near(h.viewport.camera().yaw, yaw));
    CHECK(near(h.viewport.camera().distance, distance));
    CHECK(near(h.viewport.camera().target, target));
    CHECK(h.viewport.dragging());
}

TEST_CASE("Removing the selected entity ends its drag and clears the gizmo", "[unit][editor]") {
    Harness h;
    const Guid entity = h.add({0.0, 0.0, 0.0});
    const double length = h.gizmo_length_of(entity);
    h.press(h.pixel_of(glm::dvec3{1.0, 0.0, 0.0} * (0.5 * length)));
    REQUIRE(h.viewport.dragging());

    REQUIRE(h.scene.remove(entity).ok());
    h.move_to({500.0, 400.0});
    CHECK(!h.viewport.dragging());
    CHECK(!h.scene.selected().has_value());
    CHECK(!h.viewport.gizmo().has_value());
}

TEST_CASE("A reparented selection stays selected and its gizmo follows the entity to its new place", "[unit][editor]") {
    Harness h;
    const Guid anchor = h.add({4.0, 0.0, 0.0});
    const Guid entity = h.add({0.0, 0.0, 0.0});
    h.scene.select(entity);
    REQUIRE(h.scene.reparent(entity, anchor).ok());
    h.frame(h.input({500.0, 400.0}));
    CHECK(h.scene.selected() == entity);
    REQUIRE(h.viewport.gizmo().has_value());
    CHECK(near(h.viewport.gizmo()->origin, glm::dvec3{4.0, 0.0, 0.0}, 1e-9));
}
