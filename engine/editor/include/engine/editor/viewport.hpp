#pragma once

// The scene viewport's model (US-063): a free editor camera, entity picking, and the translate, rotate, and scale gizmos with
// local or world axes and snapping. It has no ImGui or graphics dependency, so the behaviour is tested without a window
// (docs/editor.md). The viewport panel draws the grid, the markers, and the gizmo over the ImGui surface, and passes the
// pointer and key state here.
//
// Pixels are framebuffer pixels: the ImGui display is in them (imgui_input.cpp), so the panel size and the pointer need no
// conversion. The DPI scale (1.0 is 96 DPI) sizes the pick radius and the gizmo in pixels, so they keep their size on screen
// at any scale.
//
// The editor camera is not the game's camera. Its input is read here and nowhere else; no ECS camera component is read or
// written. A gizmo drag freezes the editor camera, so dragging a handle never moves the view.

#include "engine/core/guid.hpp"
#include "engine/editor/scene_editor.hpp"
#include "engine/scene/transform.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::editor {

// The size of the viewport in framebuffer pixels and the window's DPI scale.
struct ViewportSurface {
    double width = 1.0;
    double height = 1.0;
    double dpi_scale = 1.0;

    [[nodiscard]] double aspect() const noexcept {
        return height > 0.0 ? width / height : 1.0;
    }
};

// A free camera that orbits a target point. Its position is target + distance * direction(yaw, pitch), with Y up.
class EditorCamera {
public:
    glm::dvec3 target{0.0, 0.0, 0.0};
    double yaw = 0.785398163;  // radians about +Y; pi/4 looks from +X and +Z
    double pitch = 0.5;        // radians above the horizon; positive looks down at the target
    double distance = 10.0;    // metres from the eye to the target
    double fov_y = 1.0471975512;  // 60 degrees

    [[nodiscard]] glm::dvec3 position() const;
    [[nodiscard]] glm::dvec3 forward() const;
    [[nodiscard]] glm::dvec3 right() const;
    [[nodiscard]] glm::dvec3 up() const;

    // View times projection in the engine's clip convention used for picking (depth 0 to 1, y up).
    [[nodiscard]] glm::dmat4 view_projection(const ViewportSurface& surface) const;

    // Drag deltas in pixels. Orbit turns the eye about the target; pan moves the target in the view plane.
    void orbit(double dx_pixels, double dy_pixels);
    void pan(double dx_pixels, double dy_pixels, const ViewportSurface& surface);
    // Zoom: a positive value moves the eye closer. Each step scales the distance by 0.9.
    void dolly(double steps);
    // Moves the target without turning the view (fly mode and framing).
    void move(const glm::dvec3& delta) {
        target += delta;
    }
    void frame(const glm::dvec3& point) {
        target = point;
    }
};

// A world-space ray with a unit direction.
struct Ray {
    glm::dvec3 origin{0.0};
    glm::dvec3 direction{0.0, 0.0, -1.0};
};

// Projects a world point to viewport pixels (y down, origin at the top left). Nullopt for a point behind the eye or outside the depth range.
[[nodiscard]] std::optional<glm::dvec2> project_point(const glm::dmat4& view_projection, const ViewportSurface& surface,
                                                     const glm::dvec3& world);
// The ray through a pixel: it starts on the near plane and points into the scene.
[[nodiscard]] Ray camera_ray(const glm::dmat4& view_projection, const ViewportSurface& surface, const glm::dvec2& pixel);

enum class GizmoMode : std::uint8_t { Translate, Rotate, Scale };
enum class GizmoSpace : std::uint8_t { World, Local };
// X, Y, and Z are the axis handles. Centre moves in the view plane (Translate) or scales uniformly (Scale); Rotate has none.
enum class GizmoHandle : std::uint8_t { X, Y, Z, Centre };

// Snapping. Translation snaps the distance moved (metres), rotation the angle (degrees), and scale each component (factor step).
struct SnapSettings {
    bool enabled = false;
    double translate_step = 0.5;
    double rotate_step_degrees = 15.0;
    double scale_step = 0.1;
};

// Where the gizmo of the selected entity is drawn, for the frame that produced it.
struct GizmoLayout {
    core::Guid entity;
    glm::dvec3 origin{0.0};
    std::array<glm::dvec3, 3> axes{glm::dvec3{1.0, 0.0, 0.0}, glm::dvec3{0.0, 1.0, 0.0}, glm::dvec3{0.0, 0.0, 1.0}};
    double length = 1.0;  // world length of an axis handle; constant on screen
    std::optional<GizmoHandle> hovered;
    bool dragging = false;
};

// One frame of input, read from the panel by the UI. Every field is false or zero unless the UI sets it.
struct ViewportInput {
    ViewportSurface surface;
    bool hovered = false;                       // the pointer is over the viewport
    std::optional<glm::dvec2> pointer;          // pixels inside the viewport
    glm::dvec2 pointer_delta{0.0, 0.0};         // pixels moved since the last frame
    bool primary_pressed = false;               // the left button went down this frame over the viewport
    bool primary_down = false;                  // the left button is held after a press in the viewport
    bool orbit_down = false;                    // the right button is held: orbit, or fly with the move keys
    bool pan_down = false;                      // the middle button is held
    double wheel = 0.0;                         // notches; positive zooms in
    glm::dvec3 fly{0.0};                        // held move keys: x right, y up, z forward, each -1 to 1
    double delta_seconds = 0.0;
    bool frame_selection = false;               // frame the selected entity this frame
};

class EditorViewport {
public:
    [[nodiscard]] EditorCamera& camera() noexcept {
        return camera_;
    }
    [[nodiscard]] const EditorCamera& camera() const noexcept {
        return camera_;
    }

    [[nodiscard]] GizmoMode mode() const noexcept {
        return mode_;
    }
    void set_mode(GizmoMode mode) noexcept {
        mode_ = mode;
    }
    [[nodiscard]] GizmoSpace space() const noexcept {
        return space_;
    }
    void set_space(GizmoSpace space) noexcept {
        space_ = space;
    }
    [[nodiscard]] SnapSettings& snap() noexcept {
        return snap_;
    }
    [[nodiscard]] const SnapSettings& snap() const noexcept {
        return snap_;
    }

    // Runs one frame. Camera input applies unless a gizmo drag is active. A press over a gizmo handle begins a drag; a press
    // on a marker selects that entity; a press on empty space clears the selection. A drag writes the entity's local transform
    // through SceneEditor::set_transform (which logs a rejected write), and ends when the button is released, or the entity is
    // removed. Returns a message for the console when a drag cannot begin, else nullopt.
    std::optional<std::string> update(SceneEditor& scene, const ViewportInput& input);

    [[nodiscard]] bool dragging() const noexcept {
        return drag_.has_value();
    }
    // The gizmo for the selection as of the last update, or nullopt when nothing is selected.
    [[nodiscard]] const std::optional<GizmoLayout>& gizmo() const noexcept {
        return layout_;
    }
    // Points of a rotation ring around one axis, for drawing. Closed: the first point is not repeated.
    [[nodiscard]] std::vector<glm::dvec3> ring_points(std::size_t axis, std::size_t count) const;

    // The radius, in pixels at the given DPI scale, within which a press or hover hits a handle or a marker.
    [[nodiscard]] static double handle_pixels(double dpi_scale) noexcept;

    // Drops an active drag without writing anything.
    void cancel_drag() noexcept {
        drag_.reset();
    }

private:
    // An active gizmo drag. The press values are kept, so each frame's transform is computed from the press and not added to.
    struct Drag {
        core::Guid entity;
        GizmoMode mode = GizmoMode::Translate;
        GizmoSpace space = GizmoSpace::World;
        GizmoHandle handle = GizmoHandle::X;
        scene::Transform start;                // the entity's local transform at the press
        glm::dvec3 origin{0.0};                // the pivot: the entity's world position at the press
        glm::dvec3 axis{1.0, 0.0, 0.0};        // world direction of the handle; the view-plane normal for Translate Centre
        glm::dvec3 local_axis{1.0, 0.0, 0.0};  // the entity's own unit axis for the handle (rotation and scale)
        glm::dvec3 start_point{0.0};           // the press point on the handle's plane (Translate Centre and Rotate)
        double start_parameter = 0.0;          // the ray's parameter along the axis at the press (Translate axis and Scale)
        double length = 1.0;                   // the gizmo length, which normalises a scale drag
        glm::dmat3 parent_inverse{1.0};        // inverse of the parent's linear part: world vectors into the parent's frame
        glm::dquat parent_rotation{1.0, 0.0, 0.0, 0.0};
    };

    [[nodiscard]] std::optional<GizmoHandle> hit_handle(const GizmoLayout& layout, const ViewportInput& input) const;
    [[nodiscard]] std::optional<core::Guid> pick_marker(SceneEditor& scene, const ViewportInput& input) const;
    // Nullopt when the press cannot start a drag; notice then says why, when there is a reason to tell the user.
    [[nodiscard]] std::optional<Drag> begin_drag(SceneEditor& scene, const GizmoLayout& layout, GizmoHandle handle,
                                                 const ViewportInput& input, std::optional<std::string>& notice) const;
    [[nodiscard]] std::optional<scene::Transform> drag_transform(const Drag& drag, const ViewportInput& input) const;

    EditorCamera camera_;
    GizmoMode mode_ = GizmoMode::Translate;
    GizmoSpace space_ = GizmoSpace::World;
    SnapSettings snap_;
    std::optional<GizmoLayout> layout_;
    std::optional<Drag> drag_;
};

}  // namespace engine::editor
