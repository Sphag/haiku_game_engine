#pragma once

// Camera and view frustum (US-033). Positions stay double until a matrix is built, and the matrices returned here are relative to the
// camera: the camera's position is subtracted from each object before the single float conversion (docs/scene-graph.md).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>

namespace engine::render {

// Perspective camera. The orientation's identity looks down -Z with +Y up (docs/runtime-conventions.md).
struct Camera {
    glm::dvec3 position{0.0, 0.0, 0.0};
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};  // (w, x, y, z), unit length
    double fov_y_radians = 1.0471975511965976;    // 60 degrees
    double aspect = 1.0;                         // width / height of the target
    double near_plane = 0.1;
    double far_plane = 1000.0;

    [[nodiscard]] glm::dvec3 forward() const;
    // Finite, unit-length orientation, 0 < fov < pi, a positive aspect, and 0 < near < far.
    [[nodiscard]] bool is_valid() const noexcept;
};

// Clip-from-camera-relative-world matrix in engine clip space (depth 0..1, y up). Multiplying it by an object's camera-relative
// model matrix gives clip space. The result is single precision because it is what the GPU reads.
[[nodiscard]] glm::mat4 camera_view_projection(const Camera& camera);

// Six planes in the form (normal, d): a point p is inside a plane when dot(normal, p) + d >= 0. Extracted from a clip matrix for
// depth in [0, 1] (Gribb and Hartmann).
struct Frustum {
    std::array<glm::vec4, 6> planes{};
};

[[nodiscard]] Frustum make_frustum(const glm::mat4& view_projection);

// A sphere is visible when it is not entirely behind one plane. A sphere that crosses a plane counts as visible, so culling never removes
// something that might be on screen.
[[nodiscard]] bool sphere_visible(const Frustum& frustum, const glm::vec3& center, float radius) noexcept;

}  // namespace engine::render
