#pragma once

// Skinned bounds (US-036). The bounding sphere of a skinned mesh must cover it in every pose the clip can produce, so the renderer culls with
// the animated sphere rather than the bind-pose one.

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::animation {

// Vertex skin data: up to four joints per vertex, each with a weight. Weights are non-negative and sum to one.
struct SkinVertices {
    std::vector<glm::vec3> positions;  // bind-pose mesh space
    std::vector<glm::uvec4> joints;
    std::vector<glm::vec4> weights;
};

// Returns the first problem found, or nothing. The arrays must match in length, each joint must name a joint of the skeleton, and each weight
// set must be finite, non-negative, and sum to one within 1e-3.
[[nodiscard]] std::optional<std::string> validate_skin(const SkinVertices& skin, const Skeleton& skeleton);

// Normalizes a weight set in place so it sums to one. A set with no positive weight becomes (1, 0, 0, 0), so all of it goes to the vertex's first joint.
void normalize_weights(glm::vec4& weights) noexcept;

// Position of a skinned vertex in the pose's mesh space: the weighted sum of palette[joint] times the bind position.
[[nodiscard]] glm::vec3 skin_point(const glm::vec3& position, const glm::uvec4& joints, const glm::vec4& weights,
                                   const std::vector<glm::mat4>& palette) noexcept;

struct BoundsSphere {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

// A sphere that contains every skinned vertex in every sampled pose of the clip. Samples are taken at the clip start and end, at every key
// time of every channel, and at the midpoints between consecutive key times, so the poses between keys are covered by the midpoints. The
// radius is then padded by `margin`, a fraction of the radius (5 percent by default). The skin must be valid.
[[nodiscard]] BoundsSphere animated_bounds(const Skeleton& skeleton, const AnimationClip& clip, const SkinVertices& skin,
                                               float margin = 0.05f);

}  // namespace engine::animation
