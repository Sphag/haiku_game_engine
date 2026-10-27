#include "engine/animation/skeleton.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace engine::animation {
namespace {

bool finite(const glm::vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const glm::mat4& m) noexcept {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (!std::isfinite(m[c][r])) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

std::optional<std::string> validate_skeleton(const Skeleton& skeleton) {
    const std::size_t count = skeleton.parents.size();
    if (count == 0) {
        return "the skeleton has no joints";
    }
    if (skeleton.names.size() != count || skeleton.bind_pose.size() != count || skeleton.inverse_bind.size() != count) {
        return "the skeleton's name, bind pose, and inverse bind arrays do not match its joint count";
    }
    std::vector<glm::mat4> model(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::int32_t parent = skeleton.parents[i];
        if (parent < -1 || parent >= static_cast<std::int32_t>(i)) {
            return "joint " + std::to_string(i) + " has a parent that does not come before it";
        }
        const JointPose& pose = skeleton.bind_pose[i];
        if (!finite(pose.translation) || !finite(pose.scale)) {
            return "joint " + std::to_string(i) + " has a non-finite bind translation or scale";
        }
        if (pose.scale.x == 0.0f || pose.scale.y == 0.0f || pose.scale.z == 0.0f) {
            return "joint " + std::to_string(i) + " has a zero scale axis";
        }
        const float length = glm::length(pose.rotation);
        if (!std::isfinite(length) || std::abs(length - 1.0f) > 1e-3f) {
            return "joint " + std::to_string(i) + " has a bind rotation that is not unit length";
        }
        if (!finite(skeleton.inverse_bind[i])) {
            return "joint " + std::to_string(i) + " has a non-finite inverse bind matrix";
        }
        const glm::mat4 local = joint_matrix(pose);
        model[i] = parent < 0 ? local : model[static_cast<std::size_t>(parent)] * local;

        // The inverse bind matrix must undo the bind pose: their product is the identity up to float error, scaled by the transform's size.
        const glm::mat4 product = skeleton.inverse_bind[i] * model[i];
        float magnitude = 1.0f;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                magnitude = std::max(magnitude, std::abs(model[i][c][r]));
            }
        }
        const float tolerance = 1e-4f * magnitude;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                const float expected = c == r ? 1.0f : 0.0f;
                if (std::abs(product[c][r] - expected) > tolerance) {
                    return "joint " + std::to_string(i) + " has an inverse bind matrix that does not invert its bind pose";
                }
            }
        }
    }
    return std::nullopt;
}

glm::mat4 joint_matrix(const JointPose& pose) noexcept {
    const glm::mat4 translation = glm::translate(glm::mat4(1.0f), pose.translation);
    const glm::mat4 rotation = glm::mat4_cast(glm::normalize(pose.rotation));
    const glm::mat4 scale = glm::scale(glm::mat4(1.0f), pose.scale);
    return translation * rotation * scale;
}

std::vector<glm::mat4> model_pose(const Skeleton& skeleton, const std::vector<JointPose>& local) {
    std::vector<glm::mat4> model(local.size());
    for (std::size_t i = 0; i < local.size(); ++i) {
        const glm::mat4 matrix = joint_matrix(local[i]);
        const std::int32_t parent = skeleton.parents[i];
        model[i] = parent < 0 ? matrix : model[static_cast<std::size_t>(parent)] * matrix;
    }
    return model;
}

std::vector<glm::mat4> skinning_palette(const Skeleton& skeleton, const std::vector<JointPose>& local) {
    std::vector<glm::mat4> palette = model_pose(skeleton, local);
    for (std::size_t i = 0; i < palette.size(); ++i) {
        palette[i] = palette[i] * skeleton.inverse_bind[i];
    }
    return palette;
}

}  // namespace engine::animation
