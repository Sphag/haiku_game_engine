#include "engine/animation/clip.hpp"

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

bool finite(const glm::quat& q) noexcept {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

// Checks one channel's keys: finite, in [0, duration], strictly increasing in time, and (for rotations) unit length.
template <typename T, typename Check>
std::optional<std::string> validate_keys(const std::vector<Keyframe<T>>& keys, float duration, const char* channel, std::size_t joint, Check check) {
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const Keyframe<T>& key = keys[k];
        if (!std::isfinite(key.time) || key.time < 0.0f || key.time > duration) {
            return std::string("joint ") + std::to_string(joint) + " has a " + channel + " key outside [0, duration]";
        }
        if (k > 0 && !(key.time > keys[k - 1].time)) {
            return std::string("joint ") + std::to_string(joint) + " has " + channel + " keys that are not in strictly increasing time order";
        }
        if (!check(key.value)) {
            return std::string("joint ") + std::to_string(joint) + " has a non-finite " + channel + " key";
        }
    }
    return std::nullopt;
}

// Linear blend for translations and scales.
glm::vec3 lerp_vec(const glm::vec3& a, const glm::vec3& b, float t) {
    return a + (b - a) * t;
}

// Slerp along the shorter arc, normalized so repeated blends stay unit length.
glm::quat slerp_shortest(const glm::quat& a, const glm::quat& b, float t) {
    glm::quat target = b;
    if (glm::dot(a, b) < 0.0f) {
        target = -b;
    }
    return glm::normalize(glm::slerp(a, target, t));
}

// Value of a channel at a time. A periodic read of a looping clip wraps through the seam; otherwise the ends hold. The keys are valid.
template <typename T, typename Lerp>
T sample_channel(const std::vector<Keyframe<T>>& keys, const T& fallback, float time, float duration, bool periodic, Lerp lerp) {
    if (keys.empty()) {
        return fallback;
    }
    if (keys.size() == 1) {
        return keys.front().value;
    }
    const Keyframe<T>& first = keys.front();
    const Keyframe<T>& last = keys.back();
    if (time < first.time || time >= last.time) {
        if (!periodic) {
            return time < first.time ? first.value : last.value;
        }
        // The seam runs from the last key, at last.time, to the first key, one period later.
        const float span = first.time + duration - last.time;
        const float along = time < first.time ? time + duration - last.time : time - last.time;
        return lerp(last.value, first.value, std::clamp(along / span, 0.0f, 1.0f));
    }
    const auto upper = std::upper_bound(keys.begin(), keys.end(), time, [](float value, const Keyframe<T>& key) { return value < key.time; });
    const Keyframe<T>& after = *upper;
    const Keyframe<T>& before = *(upper - 1);
    return lerp(before.value, after.value, (time - before.time) / (after.time - before.time));
}

}  // namespace

std::optional<std::string> validate_clip(const AnimationClip& clip, const Skeleton& skeleton) {
    if (!std::isfinite(clip.duration) || !(clip.duration > 0.0f)) {
        return "the clip duration must be finite and greater than zero";
    }
    if (clip.tracks.size() != skeleton.joint_count()) {
        return "the clip has " + std::to_string(clip.tracks.size()) + " tracks, but the skeleton has " + std::to_string(skeleton.joint_count()) + " joints";
    }
    for (std::size_t j = 0; j < clip.tracks.size(); ++j) {
        const JointTrack& track = clip.tracks[j];
        if (auto problem = validate_keys(track.translation, clip.duration, "translation", j, [](const glm::vec3& v) { return finite(v); })) {
            return problem;
        }
        if (auto problem = validate_keys(track.scale, clip.duration, "scale", j, [](const glm::vec3& v) { return finite(v); })) {
            return problem;
        }
        if (auto problem = validate_keys(track.rotation, clip.duration, "rotation", j, [](const glm::quat& q) {
                return finite(q) && std::abs(glm::length(q) - 1.0f) <= 1e-3f;
            })) {
            return problem;
        }
    }
    return std::nullopt;
}

float wrap_time(const AnimationClip& clip, float time) noexcept {
    if (!clip.looping) {
        return std::clamp(time, 0.0f, clip.duration);
    }
    // Computed in double so a large unwrapped time keeps its fraction.
    const double period = clip.duration;
    double wrapped = std::fmod(static_cast<double>(time), period);
    if (wrapped < 0.0) {
        wrapped += period;
    }
    if (wrapped >= period) {
        wrapped = 0.0;
    }
    return static_cast<float>(wrapped);
}

JointPose sample_joint(const JointTrack& track, const JointPose& bind, float time, float duration, bool periodic) {
    JointPose pose;
    pose.translation = sample_channel(track.translation, bind.translation, time, duration, periodic, lerp_vec);
    pose.rotation = sample_channel(track.rotation, bind.rotation, time, duration, periodic, slerp_shortest);
    pose.scale = sample_channel(track.scale, bind.scale, time, duration, periodic, lerp_vec);
    return pose;
}

std::vector<JointPose> sample_pose(const Skeleton& skeleton, const AnimationClip& clip, float time) {
    const float wrapped = wrap_time(clip, time);
    std::vector<JointPose> pose(clip.tracks.size());
    for (std::size_t j = 0; j < clip.tracks.size(); ++j) {
        pose[j] = sample_joint(clip.tracks[j], skeleton.bind_pose[j], wrapped, clip.duration, clip.looping);
    }
    return pose;
}

std::vector<JointPose> blend_poses(const std::vector<JointPose>& a, const std::vector<JointPose>& b, float weight) {
    std::vector<JointPose> out(a.size());
    for (std::size_t j = 0; j < a.size(); ++j) {
        out[j].translation = lerp_vec(a[j].translation, b[j].translation, weight);
        out[j].rotation = slerp_shortest(a[j].rotation, b[j].rotation, weight);
        out[j].scale = lerp_vec(a[j].scale, b[j].scale, weight);
    }
    return out;
}

}  // namespace engine::animation
