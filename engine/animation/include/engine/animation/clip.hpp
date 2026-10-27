#pragma once

// Animation clips (US-036). A clip holds one track per joint of its skeleton. A track has keys for translation, rotation, and scale, each in
// strictly increasing time order. A channel with no keys holds the joint's bind-pose value. Translations and scales interpolate linearly and
// rotations interpolate by slerp.
//
// Looping. A looping clip is periodic with period `duration`: between the last key and the first key of a channel (wrapping through the
// end of the clip), the value interpolates across the boundary, so the pose is continuous at the loop point. A non-looping clip holds its
// first key before the start and its last key after the end, and its time is clamped to [0, duration].

#include "engine/animation/skeleton.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string>
#include <vector>

namespace engine::animation {

template <typename T>
struct Keyframe {
    float time = 0.0f;  // seconds from the clip start, in [0, duration]
    T value{};
};

struct JointTrack {
    std::vector<Keyframe<glm::vec3>> translation;
    std::vector<Keyframe<glm::quat>> rotation;
    std::vector<Keyframe<glm::vec3>> scale;
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;  // seconds, greater than zero
    bool looping = true;
    std::vector<JointTrack> tracks;  // one per joint of the skeleton, in joint order
};

// Returns the first problem found, or nothing for a clip that matches the skeleton: one track per joint, a positive finite duration, keys
// in strictly increasing time order within [0, duration], finite values, and unit-length rotations.
[[nodiscard]] std::optional<std::string> validate_clip(const AnimationClip& clip, const Skeleton& skeleton);

// The time a clip is sampled at: a looping clip wraps `time` into [0, duration); a non-looping clip clamps it.
[[nodiscard]] float wrap_time(const AnimationClip& clip, float time) noexcept;

// One joint's local pose at `time`. A periodic (looping) read wraps across the loop seam; a non-periodic read holds the first and last keys
// at the ends. Root motion reads the root joint non-periodically (see playback.hpp). The track must be valid.
[[nodiscard]] JointPose sample_joint(const JointTrack& track, const JointPose& bind, float time, float duration, bool periodic);

// The local pose of every joint at `time` (wrapped or clamped as above). The skeleton and clip must have been validated.
[[nodiscard]] std::vector<JointPose> sample_pose(const Skeleton& skeleton, const AnimationClip& clip, float time);

// Linear blend of two poses: translations and scales lerp, rotations slerp along the shorter arc. weight 0 gives `a`, and weight 1 gives `b`.
[[nodiscard]] std::vector<JointPose> blend_poses(const std::vector<JointPose>& a, const std::vector<JointPose>& b, float weight);

}  // namespace engine::animation
