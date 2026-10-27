#pragma once

// Playback (US-036): a clip player that advances time with a speed and tracks loop wraps, and root motion. Root motion takes the displacement
// of one joint (the root) out of the pose and hands it to the entity that plays the clip.
//
// Root motion. For each enabled axis the entity moves by the root's displacement from the start of the clip, and the pose holds the root at its
// start value on that axis. Over whole cycles the displacement grows by the root's per-cycle motion (the difference between its last and first
// key), so a walk cycle moves the entity forward on every loop. For yaw, the root's heading change is handed to the entity and removed from
// the pose's root rotation, so the combination of the two equals the authored root rotation exactly. The entity's transform times the pose
// therefore reproduces the authored root. Enabled axes are an input, so a clip can carry root motion on the horizontal plane only, or yaw only.

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace engine::animation {

struct RootMotion {
    std::uint32_t joint = 0;     // the root joint
    bool translate_x = true;
    bool translate_y = false;
    bool translate_z = true;
    bool yaw = true;             // rotation about the vertical axis
};

// The entity's displacement from the clip's start at an unwrapped time: the translation on the enabled axes (zero elsewhere) and the yaw.
struct RootDisplacement {
    glm::vec3 translation{0.0f};
    float yaw = 0.0f;  // radians about +Y
};

// Displacement at `unwrapped_time` seconds from the clip start. The time is not wrapped, so whole cycles accumulate. The skeleton and clip must
// be valid and the root joint must exist.
[[nodiscard]] RootDisplacement root_displacement(const Skeleton& skeleton, const AnimationClip& clip, const RootMotion& root, float unwrapped_time);

// Removes the extracted root components from a pose sampled at `time` (wrapped as sample_pose does): the enabled translation axes are held
// at the root's start value, and the yaw is removed from the root rotation.
void remove_root_motion(const Skeleton& skeleton, const AnimationClip& clip, const RootMotion& root, std::vector<JointPose>& pose, float time);

// Advances a clip. Time is kept unwrapped in `elapsed`; `time()` is the wrapped (looping) or clamped (not looping) time to sample.
class ClipPlayer {
public:
    explicit ClipPlayer(const AnimationClip& clip, float speed = 1.0f);

    // Moves by dt seconds of real time times the speed. A negative speed plays backward. dt must be finite and non-negative.
    void advance(float dt);
    void seek(float elapsed) noexcept;

    [[nodiscard]] const AnimationClip& clip() const noexcept {
        return *clip_;
    }
    [[nodiscard]] float speed() const noexcept {
        return speed_;
    }
    void set_speed(float speed) noexcept {
        speed_ = speed;
    }
    [[nodiscard]] float elapsed() const noexcept {
        return elapsed_;
    }
    [[nodiscard]] float time() const noexcept;
    // Whole loop cycles completed since the clip start (negative when played backward past it). Zero for a non-looping clip.
    [[nodiscard]] std::int64_t cycles() const noexcept;
    // True once a non-looping clip has reached its end.
    [[nodiscard]] bool finished() const noexcept;

private:
    const AnimationClip* clip_;
    float speed_;
    float elapsed_ = 0.0f;
};

// The pose and root displacement of a blend of two clips on the same skeleton. Weight 0 is clip `a` at its time and weight 1 is clip `b` at its
// time. The root displacement blends the same way. The pose has its extracted root components removed.
struct BlendSample {
    std::vector<JointPose> pose;
    RootDisplacement root;
};

[[nodiscard]] BlendSample sample_blend(const Skeleton& skeleton, const AnimationClip& a, float unwrapped_a, const AnimationClip& b,
                                       float unwrapped_b, float weight, const RootMotion& root);

}  // namespace engine::animation
