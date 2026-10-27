#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>

namespace engine::scene {

// Local transform of a node, relative to its parent (or to the world for a root). The local matrix is T * R * S: scale
// is applied first, then rotation, then translation. Components are double so that positions far from the origin keep
// their precision; rendering converts to camera-relative floats (docs/scene-graph.md).
struct Transform {
    glm::dvec3 position{0.0, 0.0, 0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};  // identity, stored as (w, x, y, z)
    glm::dvec3 scale{1.0, 1.0, 1.0};

    [[nodiscard]] glm::dmat4 matrix() const;

    // Every component is finite and the rotation has unit length. A zero or negative scale is valid.
    [[nodiscard]] bool is_valid() const noexcept;
};

// Recovers the translation, rotation, and scale of an affine matrix. Returns nullopt when the matrix cannot be
// represented exactly by a Transform: it has shear, a zero scale axis, a projective row, or a non-finite value.
// A negative determinant is kept as a negative scale on the x axis.
[[nodiscard]] std::optional<Transform> decompose_exact(const glm::dmat4& matrix);

}  // namespace engine::scene
