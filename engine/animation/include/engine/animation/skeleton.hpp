#pragma once

// Skeletons (US-036). A skeleton is a list of joints in parent-before-child order. Each joint has a bind pose (local translation,
// rotation, and scale relative to its parent) and an inverse bind matrix, which maps mesh space to the joint's space in the bind pose.
// Translations are meters, rotations are unit quaternions (w, x, y, z), and matrices are column-major with the engine's Y-up convention.
// A pose is one local JointPose per joint, in the same order.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::animation {

struct JointPose {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // unit length, (w, x, y, z)
    glm::vec3 scale{1.0f};
};

struct Skeleton {
    std::vector<std::string> names;      // one per joint, for diagnostics
    std::vector<std::int32_t> parents;   // -1 for a root; otherwise an index lower than the joint's own
    std::vector<JointPose> bind_pose;    // local transform of each joint in the bind pose
    std::vector<glm::mat4> inverse_bind; // mesh space to joint space in the bind pose

    [[nodiscard]] std::uint32_t joint_count() const noexcept {
        return static_cast<std::uint32_t>(parents.size());
    }
};

// Returns the first problem found, or nothing for a valid skeleton. Checks that the arrays agree in length, that parents come before their
// children, that every value is finite, that scales are non-zero, that rotations are unit length, and that each inverse bind matrix is the
// inverse of the joint's bind-pose model matrix (within a tolerance that scales with the transform's size).
[[nodiscard]] std::optional<std::string> validate_skeleton(const Skeleton& skeleton);

// The local transform of a pose as a matrix: translation, then rotation, then scale.
[[nodiscard]] glm::mat4 joint_matrix(const JointPose& pose) noexcept;

// Joint-space to mesh-space matrices of a pose: each joint's local matrix times its parent's model matrix. The skeleton must be valid and the
// pose must have one entry per joint.
[[nodiscard]] std::vector<glm::mat4> model_pose(const Skeleton& skeleton, const std::vector<JointPose>& local);

// The skinning palette of a pose: model matrix times inverse bind matrix, one per joint. A vertex is skinned by the weighted sum of these.
[[nodiscard]] std::vector<glm::mat4> skinning_palette(const Skeleton& skeleton, const std::vector<JointPose>& local);

}  // namespace engine::animation
