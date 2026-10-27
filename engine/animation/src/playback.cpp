#include "engine/animation/playback.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace engine::animation {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Wraps an angle into (-pi, pi].
float wrap_angle(float angle) noexcept {
    const float turns = std::round(angle / (2.0f * kPi));
    return angle - turns * 2.0f * kPi;
}

// Heading of a rotation: the angle about +Y of its forward vector, so a pure yaw by theta has heading theta.
float heading(const glm::quat& rotation) noexcept {
    const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    return std::atan2(forward.x, forward.z);
}

// The root's track read non-periodically: the cumulative motion of the root from its start.
JointPose root_at(const Skeleton& skeleton, const AnimationClip& clip, std::uint32_t joint, float time) {
    return sample_joint(clip.tracks[joint], skeleton.bind_pose[joint], time, clip.duration, false);
}

}  // namespace

RootDisplacement root_displacement(const Skeleton& skeleton, const AnimationClip& clip, const RootMotion& root, float unwrapped_time) {
    RootDisplacement out;
    const double period = clip.duration;
    double cycles = 0.0;
    float within = 0.0f;
    if (clip.looping) {
        cycles = std::floor(static_cast<double>(unwrapped_time) / period);
        within = static_cast<float>(static_cast<double>(unwrapped_time) - cycles * period);
        if (within >= clip.duration) {
            within = 0.0f;
            cycles += 1.0;
        }
    } else {
        within = std::clamp(unwrapped_time, 0.0f, clip.duration);
    }

    const JointPose start = root_at(skeleton, clip, root.joint, 0.0f);
    const JointPose end = root_at(skeleton, clip, root.joint, clip.duration);
    const JointPose now = root_at(skeleton, clip, root.joint, within);
    const glm::vec3 cycle = end.translation - start.translation;
    const glm::vec3 delta = (now.translation - start.translation) + static_cast<float>(cycles) * cycle;
    out.translation.x = root.translate_x ? delta.x : 0.0f;
    out.translation.y = root.translate_y ? delta.y : 0.0f;
    out.translation.z = root.translate_z ? delta.z : 0.0f;
    if (root.yaw) {
        const float cycle_yaw = wrap_angle(heading(end.rotation) - heading(start.rotation));
        out.yaw = wrap_angle(heading(now.rotation) - heading(start.rotation)) + static_cast<float>(cycles) * cycle_yaw;
    }
    return out;
}

void remove_root_motion(const Skeleton& skeleton, const AnimationClip& clip, const RootMotion& root, std::vector<JointPose>& pose, float time) {
    if (!root.translate_x && !root.translate_y && !root.translate_z && !root.yaw) {
        return;
    }
    const float within = wrap_time(clip, time);
    const JointPose start = root_at(skeleton, clip, root.joint, 0.0f);
    const JointPose now = root_at(skeleton, clip, root.joint, within);
    JointPose& joint = pose[root.joint];
    if (root.translate_x) {
        joint.translation.x = start.translation.x;
    }
    if (root.translate_y) {
        joint.translation.y = start.translation.y;
    }
    if (root.translate_z) {
        joint.translation.z = start.translation.z;
    }
    if (root.yaw) {
        // The heading change inside this cycle is taken out of the rotation, so the entity's yaw from root_displacement restores it.
        const float turn = wrap_angle(heading(now.rotation) - heading(start.rotation));
        joint.rotation = glm::normalize(glm::angleAxis(-turn, glm::vec3(0.0f, 1.0f, 0.0f)) * now.rotation);
    }
}

ClipPlayer::ClipPlayer(const AnimationClip& clip, float speed) : clip_(&clip), speed_(speed) {}

void ClipPlayer::advance(float dt) {
    elapsed_ += dt * speed_;
    if (!clip_->looping) {
        elapsed_ = std::clamp(elapsed_, 0.0f, clip_->duration);
    }
}

void ClipPlayer::seek(float elapsed) noexcept {
    elapsed_ = elapsed;
    if (!clip_->looping) {
        elapsed_ = std::clamp(elapsed_, 0.0f, clip_->duration);
    }
}

float ClipPlayer::time() const noexcept {
    return wrap_time(*clip_, elapsed_);
}

std::int64_t ClipPlayer::cycles() const noexcept {
    if (!clip_->looping) {
        return 0;
    }
    return static_cast<std::int64_t>(std::floor(static_cast<double>(elapsed_) / static_cast<double>(clip_->duration)));
}

bool ClipPlayer::finished() const noexcept {
    if (clip_->looping) {
        return false;
    }
    return speed_ >= 0.0f ? elapsed_ >= clip_->duration : elapsed_ <= 0.0f;
}

BlendSample sample_blend(const Skeleton& skeleton, const AnimationClip& a, float unwrapped_a, const AnimationClip& b, float unwrapped_b,
                         float weight, const RootMotion& root) {
    BlendSample out;
    std::vector<JointPose> pose_a = sample_pose(skeleton, a, unwrapped_a);
    std::vector<JointPose> pose_b = sample_pose(skeleton, b, unwrapped_b);
    remove_root_motion(skeleton, a, root, pose_a, unwrapped_a);
    remove_root_motion(skeleton, b, root, pose_b, unwrapped_b);
    out.pose = blend_poses(pose_a, pose_b, weight);

    const RootDisplacement da = root_displacement(skeleton, a, root, unwrapped_a);
    const RootDisplacement db = root_displacement(skeleton, b, root, unwrapped_b);
    out.root.translation = da.translation + (db.translation - da.translation) * weight;
    out.root.yaw = da.yaw + (db.yaw - da.yaw) * weight;
    return out;
}

}  // namespace engine::animation
