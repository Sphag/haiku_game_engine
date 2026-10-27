#include "engine/editor/viewport.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::editor {
namespace {

constexpr double kEpsilon = 1e-9;
constexpr double kTau = 6.283185307179586;
constexpr double kOrbitRadiansPerPixel = 0.005;
constexpr double kPitchLimit = 1.5;
constexpr double kMinDistance = 0.05;
constexpr double kMaxDistance = 1.0e6;
constexpr double kDollyPerStep = 0.9;
constexpr double kNearPlane = 0.05;
constexpr double kFarPlane = 1.0e6;
constexpr double kFlySpeed = 1.5;  // distances per second, scaled by the camera distance
constexpr double kHandlePixels = 8.0;
constexpr double kGizmoPixels = 90.0;  // length of an axis handle at DPI scale 1
constexpr double kMinScale = 0.001;
constexpr double kMinFactor = 0.001;
constexpr std::size_t kRingSegments = 64;

constexpr glm::dvec3 kUpAxis{0.0, 1.0, 0.0};

glm::dvec3 unit_axis(std::size_t index) {
    glm::dvec3 axis{0.0};
    axis[static_cast<int>(index)] = 1.0;
    return axis;
}

// The unit direction from the target to the eye.
glm::dvec3 eye_direction(double yaw, double pitch) {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
}

std::size_t axis_index(GizmoHandle handle) noexcept {
    switch (handle) {
    case GizmoHandle::X:
    case GizmoHandle::Centre:
        return 0;
    case GizmoHandle::Y:
        return 1;
    case GizmoHandle::Z:
        return 2;
    }
    return 0;
}

// Parameter t of the point origin + t * dir that is nearest to the ray (dir is a unit vector). Nullopt when they are parallel.
std::optional<double> line_parameter(const Ray& ray, const glm::dvec3& origin, const glm::dvec3& dir) {
    // Closest points of two lines (Ericson, Real-Time Collision Detection, 5.1.8), with both directions unit length.
    const glm::dvec3 w0 = ray.origin - origin;
    const double b = glm::dot(ray.direction, dir);
    const double d = glm::dot(ray.direction, w0);
    const double e = glm::dot(dir, w0);
    const double denom = 1.0 - b * b;
    if (std::abs(denom) < kEpsilon) {
        return std::nullopt;
    }
    return (e - b * d) / denom;
}

// The ray's hit with a plane, at or after the ray's start. Nullopt when the ray is parallel to the plane or points away from it.
std::optional<glm::dvec3> plane_hit(const Ray& ray, const glm::dvec3& point, const glm::dvec3& normal) {
    const double facing = glm::dot(ray.direction, normal);
    if (std::abs(facing) < kEpsilon) {
        return std::nullopt;
    }
    const double s = glm::dot(point - ray.origin, normal) / facing;
    if (s < 0.0) {
        return std::nullopt;
    }
    return ray.origin + s * ray.direction;
}

double snap_value(double value, double step) {
    return step > kEpsilon ? std::round(value / step) * step : value;
}

// The angle from one unit vector to another, about an axis they both lie around the plane of.
double signed_angle(const glm::dvec3& from, const glm::dvec3& to, const glm::dvec3& axis) {
    return std::atan2(glm::dot(glm::cross(from, to), axis), glm::dot(from, to));
}

std::array<glm::dvec3, 2> basis_perpendicular(const glm::dvec3& axis) {
    const glm::dvec3 helper = std::abs(axis.y) < 0.9 ? kUpAxis : glm::dvec3{1.0, 0.0, 0.0};
    const glm::dvec3 u = glm::normalize(glm::cross(axis, helper));
    return {u, glm::cross(axis, u)};
}

std::vector<glm::dvec3> ring_around(const glm::dvec3& origin, const glm::dvec3& axis, double radius, std::size_t count) {
    const std::array<glm::dvec3, 2> basis = basis_perpendicular(axis);
    std::vector<glm::dvec3> points;
    points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double angle = kTau * static_cast<double>(i) / static_cast<double>(count);
        points.push_back(origin + radius * (std::cos(angle) * basis[0] + std::sin(angle) * basis[1]));
    }
    return points;
}

double segment_distance(const glm::dvec2& point, const glm::dvec2& a, const glm::dvec2& b) {
    const glm::dvec2 ab = b - a;
    const double length_squared = glm::dot(ab, ab);
    const double t = length_squared > kEpsilon ? std::clamp(glm::dot(point - a, ab) / length_squared, 0.0, 1.0) : 0.0;
    return glm::length(point - (a + t * ab));
}

// The handle directions for a space: the world axes, or the entity's own axes (its world linear columns, normalised).
std::array<glm::dvec3, 3> handle_axes(const glm::dmat4& world, GizmoSpace space) {
    std::array<glm::dvec3, 3> axes{};
    for (std::size_t i = 0; i < 3; ++i) {
        const glm::dvec3 column = glm::dvec3(world[static_cast<int>(i)]);
        const bool usable = space == GizmoSpace::Local && glm::length(column) > kEpsilon;
        axes[i] = usable ? glm::normalize(column) : unit_axis(i);
    }
    return axes;
}

// The gizmo for the selected entity, or nullopt when nothing is selected. Scale always works on the entity's own axes, in
// either space: a world-space scale of a rotated entity is not a diagonal transform.
std::optional<GizmoLayout> build_layout(SceneEditor& scene, const EditorCamera& camera, const ViewportSurface& surface,
                                        GizmoMode mode, GizmoSpace space) {
    const std::optional<core::Guid> selected = scene.selected();
    if (!selected) {
        return std::nullopt;
    }
    const std::optional<glm::dmat4> world = scene.world_matrix(*selected);
    if (!world) {
        return std::nullopt;
    }
    GizmoLayout layout;
    layout.entity = *selected;
    layout.origin = glm::dvec3((*world)[3]);
    layout.axes = handle_axes(*world, mode == GizmoMode::Scale ? GizmoSpace::Local : space);
    // The handle is a fixed number of pixels long at any distance: convert pixels to metres at the origin's depth.
    const double distance = std::max(glm::length(layout.origin - camera.position()), kMinDistance);
    const double metres_per_pixel = 2.0 * distance * std::tan(camera.fov_y * 0.5) / surface.height;
    layout.length = kGizmoPixels * surface.dpi_scale * metres_per_pixel;
    return layout;
}

}  // namespace

glm::dvec3 EditorCamera::position() const {
    return target + distance * eye_direction(yaw, pitch);
}

glm::dvec3 EditorCamera::forward() const {
    return -eye_direction(yaw, pitch);
}

glm::dvec3 EditorCamera::right() const {
    return glm::normalize(glm::cross(forward(), kUpAxis));
}

glm::dvec3 EditorCamera::up() const {
    return glm::cross(right(), forward());
}

glm::dmat4 EditorCamera::view_projection(const ViewportSurface& surface) const {
    const glm::dmat4 view = glm::lookAtRH(position(), target, kUpAxis);
    const glm::dmat4 projection = glm::perspectiveRH_ZO(fov_y, surface.aspect(), kNearPlane, kFarPlane);
    return projection * view;
}

void EditorCamera::orbit(double dx_pixels, double dy_pixels) {
    yaw -= dx_pixels * kOrbitRadiansPerPixel;
    pitch = std::clamp(pitch + dy_pixels * kOrbitRadiansPerPixel, -kPitchLimit, kPitchLimit);
}

void EditorCamera::pan(double dx_pixels, double dy_pixels, const ViewportSurface& surface) {
    if (surface.height <= 0.0) {
        return;
    }
    // The scene follows the pointer: a drag right moves the target left, and a drag down moves it up.
    const double metres_per_pixel = 2.0 * distance * std::tan(fov_y * 0.5) / surface.height;
    target += (-dx_pixels * right() + dy_pixels * up()) * metres_per_pixel;
}

void EditorCamera::dolly(double steps) {
    distance = std::clamp(distance * std::pow(kDollyPerStep, steps), kMinDistance, kMaxDistance);
}

std::optional<glm::dvec2> project_point(const glm::dmat4& view_projection, const ViewportSurface& surface, const glm::dvec3& world) {
    const glm::dvec4 clip = view_projection * glm::dvec4(world, 1.0);
    if (clip.w <= kEpsilon) {
        return std::nullopt;
    }
    const glm::dvec3 ndc = glm::dvec3(clip) / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) {
        return std::nullopt;
    }
    return glm::dvec2((ndc.x + 1.0) * 0.5 * surface.width, (1.0 - ndc.y) * 0.5 * surface.height);
}

Ray camera_ray(const glm::dmat4& view_projection, const ViewportSurface& surface, const glm::dvec2& pixel) {
    const glm::dmat4 inverse = glm::inverse(view_projection);
    const double x = pixel.x / surface.width * 2.0 - 1.0;
    const double y = 1.0 - pixel.y / surface.height * 2.0;
    const glm::dvec4 near_clip = inverse * glm::dvec4(x, y, 0.0, 1.0);
    const glm::dvec4 far_clip = inverse * glm::dvec4(x, y, 1.0, 1.0);
    const glm::dvec3 near_point = glm::dvec3(near_clip) / near_clip.w;
    const glm::dvec3 far_point = glm::dvec3(far_clip) / far_clip.w;
    return Ray{near_point, glm::normalize(far_point - near_point)};
}

double EditorViewport::handle_pixels(double dpi_scale) noexcept {
    return kHandlePixels * dpi_scale;
}

std::vector<glm::dvec3> EditorViewport::ring_points(std::size_t axis, std::size_t count) const {
    if (!layout_ || axis >= 3 || count < 3) {
        return {};
    }
    return ring_around(layout_->origin, layout_->axes[axis], layout_->length, count);
}

std::optional<GizmoHandle> EditorViewport::hit_handle(const GizmoLayout& layout, const ViewportInput& input) const {
    if (!input.pointer) {
        return std::nullopt;
    }
    const glm::dmat4 view_projection = camera_.view_projection(input.surface);
    const glm::dvec2 pointer = *input.pointer;
    const double reach = handle_pixels(input.surface.dpi_scale);
    const std::optional<glm::dvec2> origin_px = project_point(view_projection, input.surface, layout.origin);
    if (!origin_px) {
        return std::nullopt;
    }
    // The centre wins over the axes it sits on, so a press at the pivot moves in the view plane or scales uniformly.
    if (mode_ != GizmoMode::Rotate && glm::length(pointer - *origin_px) < reach) {
        return GizmoHandle::Centre;
    }
    std::optional<GizmoHandle> best;
    double best_distance = reach;
    const GizmoHandle handles[3] = {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z};
    for (std::size_t i = 0; i < 3; ++i) {
        double distance = std::numeric_limits<double>::infinity();
        if (mode_ == GizmoMode::Rotate) {
            const std::vector<glm::dvec3> ring = ring_around(layout.origin, layout.axes[i], layout.length, kRingSegments);
            for (std::size_t k = 0; k < ring.size(); ++k) {
                const std::optional<glm::dvec2> a = project_point(view_projection, input.surface, ring[k]);
                const std::optional<glm::dvec2> b = project_point(view_projection, input.surface, ring[(k + 1) % ring.size()]);
                if (a && b) {
                    distance = std::min(distance, segment_distance(pointer, *a, *b));
                }
            }
        } else {
            const std::optional<glm::dvec2> tip =
                project_point(view_projection, input.surface, layout.origin + layout.axes[i] * layout.length);
            if (tip) {
                distance = segment_distance(pointer, *origin_px, *tip);
            }
        }
        if (distance < best_distance) {
            best = handles[i];
            best_distance = distance;
        }
    }
    return best;
}

std::optional<core::Guid> EditorViewport::pick_marker(SceneEditor& scene, const ViewportInput& input) const {
    if (!input.pointer) {
        return std::nullopt;
    }
    const glm::dmat4 view_projection = camera_.view_projection(input.surface);
    const double reach = handle_pixels(input.surface.dpi_scale);
    std::optional<core::Guid> best;
    double best_distance = reach;
    for (const HierarchyRow& row : scene.hierarchy("")) {
        const std::optional<glm::dmat4> world = scene.world_matrix(row.guid);
        if (!world) {
            continue;
        }
        const std::optional<glm::dvec2> marker = project_point(view_projection, input.surface, glm::dvec3((*world)[3]));
        if (!marker) {
            continue;
        }
        const double distance = glm::length(*marker - *input.pointer);
        if (distance < best_distance) {
            best = row.guid;
            best_distance = distance;
        }
    }
    return best;
}

std::optional<EditorViewport::Drag> EditorViewport::begin_drag(SceneEditor& scene, const GizmoLayout& layout, GizmoHandle handle,
                                                               const ViewportInput& input, std::optional<std::string>& notice) const {
    if (!input.pointer) {
        return std::nullopt;
    }
    const scene::Transform* authored = scene.scene().transform(layout.entity);
    if (authored == nullptr) {
        return std::nullopt;
    }
    const core::Guid parent_guid = scene.scene().parent(layout.entity);
    glm::dmat4 parent_world{1.0};
    if (!parent_guid.is_nil()) {
        const std::optional<glm::dmat4> found = scene.world_matrix(parent_guid);
        if (!found) {
            return std::nullopt;
        }
        parent_world = *found;
    }
    const glm::dmat3 parent_linear(parent_world);
    const std::optional<scene::Transform> parent_transform = scene::decompose_exact(parent_world);
    if (!parent_transform || std::abs(glm::determinant(parent_linear)) < kEpsilon) {
        notice = "The gizmo cannot drag this entity: its parent's transform has no exact inverse.";
        return std::nullopt;
    }

    Drag drag;
    drag.entity = layout.entity;
    drag.mode = mode_;
    drag.space = space_;
    drag.handle = handle;
    drag.start = *authored;
    drag.origin = layout.origin;
    drag.length = layout.length;
    drag.parent_inverse = glm::inverse(parent_linear);
    drag.parent_rotation = parent_transform->rotation;
    const std::size_t index = axis_index(handle);
    drag.axis = layout.axes[index];
    drag.local_axis = unit_axis(index);

    const glm::dmat4 view_projection = camera_.view_projection(input.surface);
    const Ray ray = camera_ray(view_projection, input.surface, *input.pointer);
    switch (mode_) {
    case GizmoMode::Translate:
        if (handle == GizmoHandle::Centre) {
            drag.axis = -camera_.forward();
            const std::optional<glm::dvec3> hit = plane_hit(ray, drag.origin, drag.axis);
            if (!hit) {
                return std::nullopt;
            }
            drag.start_point = *hit;
        } else {
            const std::optional<double> t = line_parameter(ray, drag.origin, drag.axis);
            if (!t) {
                return std::nullopt;
            }
            drag.start_parameter = *t;
        }
        break;
    case GizmoMode::Rotate: {
        if (handle == GizmoHandle::Centre) {
            return std::nullopt;
        }
        const std::optional<glm::dvec3> hit = plane_hit(ray, drag.origin, drag.axis);
        if (!hit || glm::length(*hit - drag.origin) < kEpsilon) {
            return std::nullopt;
        }
        drag.start_point = *hit;
        break;
    }
    case GizmoMode::Scale: {
        if (handle == GizmoHandle::Centre) {
            // Uniform scale: the direction from the pivot to the press, within the view plane.
            const std::optional<glm::dvec3> hit = plane_hit(ray, drag.origin, -camera_.forward());
            if (!hit) {
                return std::nullopt;
            }
            const glm::dvec3 offset = *hit - drag.origin;
            drag.axis = glm::length(offset) > kEpsilon ? glm::normalize(offset) : camera_.right();
        }
        const std::optional<double> t = line_parameter(ray, drag.origin, drag.axis);
        if (!t) {
            return std::nullopt;
        }
        drag.start_parameter = *t;
        break;
    }
    }
    return drag;
}

std::optional<scene::Transform> EditorViewport::drag_transform(const Drag& drag, const ViewportInput& input) const {
    if (!input.pointer) {
        return std::nullopt;
    }
    const Ray ray = camera_ray(camera_.view_projection(input.surface), input.surface, *input.pointer);
    scene::Transform next = drag.start;
    const SnapSettings& snap = snap_;
    switch (drag.mode) {
    case GizmoMode::Translate: {
        glm::dvec3 world_delta{0.0};
        if (drag.handle == GizmoHandle::Centre) {
            const std::optional<glm::dvec3> hit = plane_hit(ray, drag.origin, drag.axis);
            if (!hit) {
                return std::nullopt;
            }
            world_delta = *hit - drag.start_point;
            if (snap.enabled) {
                for (std::size_t i = 0; i < 3; ++i) {
                    world_delta[static_cast<int>(i)] = snap_value(world_delta[static_cast<int>(i)], snap.translate_step);
                }
            }
        } else {
            const std::optional<double> t = line_parameter(ray, drag.origin, drag.axis);
            if (!t) {
                return std::nullopt;
            }
            double moved = *t - drag.start_parameter;
            if (snap.enabled) {
                moved = snap_value(moved, snap.translate_step);
            }
            world_delta = drag.axis * moved;
        }
        // The delta is in world space; the local position is the parent's inverse applied to it.
        next.position = drag.start.position + glm::dvec3(drag.parent_inverse * world_delta);
        return next;
    }
    case GizmoMode::Rotate: {
        const std::optional<glm::dvec3> hit = plane_hit(ray, drag.origin, drag.axis);
        if (!hit || glm::length(*hit - drag.origin) < kEpsilon) {
            return std::nullopt;
        }
        const double angle = signed_angle(glm::normalize(drag.start_point - drag.origin), glm::normalize(*hit - drag.origin), drag.axis);
        double degrees = glm::degrees(angle);
        if (snap.enabled) {
            degrees = snap_value(degrees, snap.rotate_step_degrees);
        }
        const double radians = glm::radians(degrees);
        if (drag.space == GizmoSpace::Local) {
            next.rotation = glm::normalize(drag.start.rotation * glm::angleAxis(radians, drag.local_axis));
        } else {
            // A turn about a world axis, conjugated into the parent's frame and applied to the entity's own rotation.
            const glm::dquat turn = glm::angleAxis(radians, drag.axis);
            next.rotation = glm::normalize(glm::inverse(drag.parent_rotation) * turn * drag.parent_rotation * drag.start.rotation);
        }
        return next;
    }
    case GizmoMode::Scale: {
        const std::optional<double> t = line_parameter(ray, drag.origin, drag.axis);
        if (!t) {
            return std::nullopt;
        }
        // The factor is 1 at the press and grows by one for each gizmo length the handle is dragged outward.
        const double factor = std::max(kMinFactor, 1.0 + (*t - drag.start_parameter) / drag.length);
        const std::size_t index = axis_index(drag.handle);
        for (std::size_t i = 0; i < 3; ++i) {
            if (drag.handle != GizmoHandle::Centre && i != index) {
                continue;
            }
            double value = drag.start.scale[static_cast<int>(i)] * factor;
            if (snap.enabled) {
                value = snap_value(value, snap.scale_step);
            }
            next.scale[static_cast<int>(i)] = std::max(value, kMinScale);
        }
        return next;
    }
    }
    return std::nullopt;
}

std::optional<std::string> EditorViewport::update(SceneEditor& scene, const ViewportInput& input) {
    const ViewportSurface& surface = input.surface;
    // A collapsed panel draws nothing and takes no input. The comparisons are false for NaN too.
    if (!(surface.width >= 1.0 && surface.height >= 1.0)) {
        return std::nullopt;
    }
    // A drag is one undo step: its transform writes merge into the gesture that starts with the press and ends with the drag.
    if (drag_ && !scene.scene().contains(drag_->entity)) {
        drag_.reset();  // the entity was removed under the drag, so the selection is cleared and the drag ends with it
        scene.end_gesture();
    }
    if (drag_ && !input.primary_down) {
        drag_.reset();
        scene.end_gesture();
    }
    if (drag_) {
        // The camera is frozen while a handle is dragged, so the pointer's ray stays the one the drag started from.
        if (const std::optional<scene::Transform> next = drag_transform(*drag_, input)) {
            // A rejected write is logged by SceneEditor; the drag then ends and the entity keeps its last accepted transform.
            if (!scene.set_transform(drag_->entity, *next).ok()) {
                drag_.reset();
                scene.end_gesture();
            }
        }
        layout_ = build_layout(scene, camera_, surface, mode_, space_);
        if (layout_ && drag_) {
            layout_->dragging = true;
            layout_->hovered = drag_->handle;  // the handle being dragged stays highlighted
        }
        return std::nullopt;
    }

    if (input.frame_selection) {
        if (const std::optional<core::Guid> selected = scene.selected()) {
            if (const std::optional<glm::dmat4> world = scene.world_matrix(*selected)) {
                camera_.frame(glm::dvec3((*world)[3]));
            }
        }
    }
    if (input.orbit_down) {
        camera_.orbit(input.pointer_delta.x, input.pointer_delta.y);
        if (glm::length(input.fly) > 0.0) {
            // Fly: the held keys move the target in the camera's frame, at a speed set by the distance.
            const glm::dvec3 world = camera_.right() * input.fly.x + camera_.up() * input.fly.y + camera_.forward() * input.fly.z;
            camera_.move(world * camera_.distance * kFlySpeed * input.delta_seconds);
        }
    }
    if (input.pan_down) {
        camera_.pan(input.pointer_delta.x, input.pointer_delta.y, surface);
    }
    if (input.hovered && input.wheel != 0.0) {
        camera_.dolly(input.wheel);
    }

    layout_ = build_layout(scene, camera_, surface, mode_, space_);
    if (layout_ && input.hovered && input.pointer) {
        layout_->hovered = hit_handle(*layout_, input);
    }

    std::optional<std::string> notice;
    if (input.hovered && input.primary_pressed && input.pointer) {
        if (layout_ && layout_->hovered) {
            drag_ = begin_drag(scene, *layout_, *layout_->hovered, input, notice);
            layout_->dragging = drag_.has_value();
            if (drag_) {
                scene.begin_gesture();
            }
        } else {
            scene.select(pick_marker(scene, input));
            layout_ = build_layout(scene, camera_, surface, mode_, space_);
        }
    }
    return notice;
}

}  // namespace engine::editor
