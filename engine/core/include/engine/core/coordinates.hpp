#pragma once

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Engine spatial conventions (details in docs/runtime-conventions.md):
//   - World: right-handed, Y up, meters. The camera looks down -Z; +X is right.
//   - Matrices: column-major (m[column][row]), column vectors, so v' = M * v. A child-to-world transform is
//     parent * local, which applies the local transform first.
//   - Quaternions: unit length. q * v rotates v by q (v' = q v q*). Positive angles follow the right-hand rule.
//   - Clip space: NDC x right, y up, depth in [0, 1] with near at 0 and far at 1. D3D12 uses this directly.
//     Vulkan's NDC has y pointing down, so Vulkan consumes to_vulkan_clip() of an engine projection.
namespace engine::core::coords {

inline const glm::vec3 kRight{1.0f, 0.0f, 0.0f};
inline const glm::vec3 kUp{0.0f, 1.0f, 0.0f};
inline const glm::vec3 kBack{0.0f, 0.0f, 1.0f};
inline const glm::vec3 kForward = -kBack;

// Right-handed perspective with engine clip space (depth 0..1, y up).
[[nodiscard]] inline glm::mat4 perspective(float fov_y_radians, float aspect, float near_z, float far_z) {
    return glm::perspectiveRH_ZO(fov_y_radians, aspect, near_z, far_z);
}

// Right-handed orthographic projection with engine clip space.
[[nodiscard]] inline glm::mat4 orthographic(float left, float right, float bottom, float top, float near_z,
                                            float far_z) {
    return glm::orthoRH_ZO(left, right, bottom, top, near_z, far_z);
}

// Converts an engine projection to Vulkan clip space by flipping Y: diag(1, -1, 1, 1) * engine_clip.
[[nodiscard]] inline glm::mat4 to_vulkan_clip(const glm::mat4& engine_clip) {
    glm::mat4 flip(1.0f);
    flip[1][1] = -1.0f;
    return flip * engine_clip;
}

// Perspective divide: clip position to normalized device coordinates.
[[nodiscard]] inline glm::vec3 to_ndc(const glm::vec4& clip) {
    return glm::vec3(clip) / clip.w;
}

}  // namespace engine::core::coords
