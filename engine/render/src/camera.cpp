#include "engine/render/camera.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cmath>

namespace engine::render {

glm::dvec3 Camera::forward() const {
    return orientation * glm::dvec3(0.0, 0.0, -1.0);
}

bool Camera::is_valid() const noexcept {
    const double length = glm::length(orientation);
    const bool unit = std::abs(length - 1.0) < 1e-9;
    const bool finite = std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
    return finite && unit && fov_y_radians > 0.0 && fov_y_radians < 3.141592653589793 && aspect > 0.0 && near_plane > 0.0 &&
           far_plane > near_plane && std::isfinite(aspect) && std::isfinite(far_plane);
}

glm::mat4 camera_view_projection(const Camera& camera) {
    // The view matrix only rotates: positions are already relative to the camera, so the translation is left out.
    const glm::dmat4 rotation = glm::mat4_cast(glm::conjugate(camera.orientation));
    const glm::dmat4 projection = glm::perspectiveRH_ZO(camera.fov_y_radians, camera.aspect, camera.near_plane, camera.far_plane);
    return glm::mat4(projection * rotation);
}

Frustum make_frustum(const glm::mat4& m) {
    // Row i of the matrix. glm stores columns, so row i is the i-th component of every column.
    const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);
    Frustum frustum;
    frustum.planes[0] = row3 + row0;  // left
    frustum.planes[1] = row3 - row0;  // right
    frustum.planes[2] = row3 + row1;  // bottom
    frustum.planes[3] = row3 - row1;  // top
    frustum.planes[4] = row2;         // near: depth 0 in engine clip space
    frustum.planes[5] = row3 - row2;  // far: depth 1
    for (glm::vec4& plane : frustum.planes) {
        const float length = glm::length(glm::vec3(plane));
        if (length > 0.0f) {
            plane /= length;
        }
    }
    return frustum;
}

bool sphere_visible(const Frustum& frustum, const glm::vec3& center, float radius) noexcept {
    for (const glm::vec4& plane : frustum.planes) {
        if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
            return false;
        }
    }
    return true;
}

}  // namespace engine::render
