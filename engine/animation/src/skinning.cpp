#include "engine/animation/skinning.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace engine::animation {

std::optional<std::string> validate_skin(const SkinVertices& skin, const Skeleton& skeleton) {
    if (skin.joints.size() != skin.positions.size() || skin.weights.size() != skin.positions.size()) {
        return "the skin's joint and weight arrays do not match its vertex count";
    }
    for (std::size_t v = 0; v < skin.positions.size(); ++v) {
        const glm::vec3& p = skin.positions[v];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            return "vertex " + std::to_string(v) + " has a non-finite position";
        }
        const glm::uvec4& joints = skin.joints[v];
        const glm::vec4& weights = skin.weights[v];
        float sum = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (!std::isfinite(weights[i]) || weights[i] < 0.0f) {
                return "vertex " + std::to_string(v) + " has a negative or non-finite weight";
            }
            if (weights[i] > 0.0f && joints[i] >= skeleton.joint_count()) {
                return "vertex " + std::to_string(v) + " names joint " + std::to_string(joints[i]) + ", which the skeleton does not have";
            }
            sum += weights[i];
        }
        if (std::abs(sum - 1.0f) > 1e-3f) {
            return "vertex " + std::to_string(v) + " has weights that do not sum to one";
        }
    }
    return std::nullopt;
}

void normalize_weights(glm::vec4& weights) noexcept {
    const float sum = weights.x + weights.y + weights.z + weights.w;
    if (sum > 0.0f) {
        weights /= sum;
    } else {
        weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    }
}

glm::vec3 skin_point(const glm::vec3& position, const glm::uvec4& joints, const glm::vec4& weights,
                     const std::vector<glm::mat4>& palette) noexcept {
    glm::vec3 out(0.0f);
    for (int i = 0; i < 4; ++i) {
        if (weights[i] > 0.0f) {
            out += weights[i] * glm::vec3(palette[joints[i]] * glm::vec4(position, 1.0f));
        }
    }
    return out;
}

BoundsSphere animated_bounds(const Skeleton& skeleton, const AnimationClip& clip, const SkinVertices& skin, float margin) {
    // Sample times: the clip's ends, every key time, and the midpoint of each gap between neighbors.
    std::vector<float> times{0.0f, clip.duration};
    for (const JointTrack& track : clip.tracks) {
        for (const auto& key : track.translation) {
            times.push_back(key.time);
        }
        for (const auto& key : track.rotation) {
            times.push_back(key.time);
        }
        for (const auto& key : track.scale) {
            times.push_back(key.time);
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    const std::size_t keyed = times.size();
    for (std::size_t i = 0; i + 1 < keyed; ++i) {
        times.push_back(0.5f * (times[i] + times[i + 1]));
    }

    glm::vec3 low(std::numeric_limits<float>::max());
    glm::vec3 high(-std::numeric_limits<float>::max());
    std::vector<glm::vec3> sampled;
    sampled.reserve(times.size() * skin.positions.size());
    for (const float time : times) {
        const std::vector<glm::mat4> palette = skinning_palette(skeleton, sample_pose(skeleton, clip, time));
        for (std::size_t v = 0; v < skin.positions.size(); ++v) {
            const glm::vec3 p = skin_point(skin.positions[v], skin.joints[v], skin.weights[v], palette);
            low = glm::min(low, p);
            high = glm::max(high, p);
            sampled.push_back(p);
        }
    }

    BoundsSphere sphere;
    if (sampled.empty()) {
        return sphere;
    }
    sphere.center = 0.5f * (low + high);
    float radius = 0.0f;
    for (const glm::vec3& p : sampled) {
        radius = std::max(radius, glm::length(p - sphere.center));
    }
    sphere.radius = radius * (1.0f + margin);
    return sphere;
}

}  // namespace engine::animation
