#include "engine/scene/transform.hpp"

#include <algorithm>
#include <cmath>

namespace engine::scene {
namespace {

// Relative tolerance for the linear part of a decomposition, measured against the largest entry (with an absolute floor
// of 1). Translation is copied, not recomposed, so it never needs one.
constexpr double kExactTolerance = 1e-9;

// Squared rotation length may differ from 1 by this much before the quaternion counts as non-unit.
constexpr double kUnitLengthTolerance = 1e-9;

bool is_finite(const glm::dvec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

glm::dmat4 Transform::matrix() const {
    glm::dmat4 result = glm::mat4_cast(rotation);
    // R * S: column i of R is scaled by s_i. Multiplying by T then only sets the translation column.
    result[0] *= scale.x;
    result[1] *= scale.y;
    result[2] *= scale.z;
    result[3] = glm::dvec4(position, 1.0);
    return result;
}

bool Transform::is_valid() const noexcept {
    // A NaN or infinite quaternion fails the comparison, so one test covers both the finiteness and the unit length.
    const double length_squared = glm::dot(rotation, rotation);
    return is_finite(position) && is_finite(scale) && std::abs(length_squared - 1.0) <= kUnitLengthTolerance;
}

std::optional<Transform> decompose_exact(const glm::dmat4& matrix) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return std::nullopt;
            }
        }
    }
    // Affine only: the bottom row must be (0, 0, 0, 1).
    if (std::abs(matrix[0][3]) > kExactTolerance || std::abs(matrix[1][3]) > kExactTolerance ||
        std::abs(matrix[2][3]) > kExactTolerance || std::abs(matrix[3][3] - 1.0) > kExactTolerance) {
        return std::nullopt;
    }

    const glm::dvec3 column0{matrix[0]};
    const glm::dvec3 column1{matrix[1]};
    const glm::dvec3 column2{matrix[2]};

    // A negative determinant means the basis is mirrored. The mirror is kept as a negative x scale, so the rotation stays proper.
    const double sign = glm::dot(column0, glm::cross(column1, column2)) < 0.0 ? -1.0 : 1.0;
    const glm::dvec3 lengths{glm::length(column0), glm::length(column1), glm::length(column2)};
    if (!(lengths.x > 0.0 && lengths.y > 0.0 && lengths.z > 0.0)) {
        return std::nullopt;  // a zero axis cannot be rotated back into a unit axis
    }

    const glm::dvec3 scale{sign * lengths.x, lengths.y, lengths.z};
    const glm::dmat3 rotation_matrix{column0 / scale.x, column1 / scale.y, column2 / scale.z};

    Transform result;
    result.position = glm::dvec3{matrix[3]};
    result.rotation = glm::normalize(glm::quat_cast(rotation_matrix));
    result.scale = scale;
    if (!result.is_valid()) {
        return std::nullopt;
    }

    // Recomposition proves exactness. Shear and non-orthogonal axes change the linear part, so they fail here.
    const glm::dmat4 rebuilt = result.matrix();
    double largest = 1.0;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            largest = std::max(largest, std::abs(matrix[column][row]));
        }
    }
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            if (std::abs(rebuilt[column][row] - matrix[column][row]) > kExactTolerance * largest) {
                return std::nullopt;
            }
        }
    }
    return result;
}

}  // namespace engine::scene
