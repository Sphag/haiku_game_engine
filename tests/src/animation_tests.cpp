// Animation playback (US-036), on the CPU. These fixtures use small skeletons and clips whose poses are known in closed form, so the
// expected values come from the clip definitions, not from the sampler. They cover the reference (bind) pose, key interpolation, loop seams,
// playback time, root motion (including its reconstruction of the authored root), two-clip blends, and the animated bounds.

#include "engine/animation/clip.hpp"
#include "engine/animation/playback.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/skinning.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace engine::animation;

namespace {

using Catch::Matchers::WithinAbs;

constexpr float kPi = 3.14159265358979323846f;

// Two joints: a root at the origin, and a child 1 m up the root's Y axis. Both bind poses are the identity except the child's offset.
Skeleton two_joint_skeleton() {
    Skeleton skeleton;
    skeleton.names = {"root", "child"};
    skeleton.parents = {-1, 0};
    skeleton.bind_pose = {JointPose{}, JointPose{glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)}};
    const glm::mat4 root_model(1.0f);
    const glm::mat4 child_model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    skeleton.inverse_bind = {glm::inverse(root_model), glm::inverse(child_model)};
    return skeleton;
}

// A clip with a root that moves along x, and a child with no keys (it holds its bind pose).
AnimationClip walking_clip(float duration, bool looping) {
    AnimationClip clip;
    clip.name = "walk";
    clip.duration = duration;
    clip.looping = looping;
    clip.tracks.resize(2);
    // The root's x goes 0 -> 1 at mid-clip and 0 -> 2 at the end, so each cycle adds 2 m. The root's rotation is a yaw from 0 to 90 degrees.
    clip.tracks[0].translation = {{0.0f, glm::vec3(0.0f)}, {duration * 0.5f, glm::vec3(1.0f, 0.0f, 0.0f)}, {duration * 0.75f, glm::vec3(2.0f, 0.0f, 0.0f)}};
    clip.tracks[0].rotation = {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)},
                               {duration, glm::angleAxis(kPi * 0.5f, glm::vec3(0.0f, 1.0f, 0.0f))}};
    return clip;
}

// Heading of a rotation: the angle about +Y of its forward vector.
float heading(const glm::quat& q) {
    const glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, 1.0f);
    return std::atan2(forward.x, forward.z);
}

}  // namespace

TEST_CASE("A valid skeleton passes, and a skeleton that breaks an invariant names the problem", "[unit]") {
    Skeleton skeleton = two_joint_skeleton();
    CHECK_FALSE(validate_skeleton(skeleton).has_value());

    Skeleton reversed = skeleton;
    reversed.parents = {1, -1};  // a parent that comes after its child
    CHECK(validate_skeleton(reversed).has_value());

    Skeleton wrong_inverse = skeleton;
    wrong_inverse.inverse_bind[1] = glm::mat4(1.0f);  // does not undo the child's bind pose
    CHECK(validate_skeleton(wrong_inverse).has_value());

    Skeleton flat = skeleton;
    flat.bind_pose[1].scale = glm::vec3(0.0f, 1.0f, 1.0f);
    CHECK(validate_skeleton(flat).has_value());

    Skeleton short_names = skeleton;
    short_names.names.pop_back();
    CHECK(validate_skeleton(short_names).has_value());
}

TEST_CASE("A clip must have one track per joint, positive duration, and keys in strictly increasing order", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    CHECK_FALSE(validate_clip(walking_clip(2.0f, true), skeleton).has_value());

    AnimationClip missing_track = walking_clip(2.0f, true);
    missing_track.tracks.pop_back();
    CHECK(validate_clip(missing_track, skeleton).has_value());

    AnimationClip unsorted = walking_clip(2.0f, true);
    unsorted.tracks[0].translation = {{1.0f, glm::vec3(0.0f)}, {0.5f, glm::vec3(1.0f)}};
    CHECK(validate_clip(unsorted, skeleton).has_value());

    AnimationClip late_key = walking_clip(2.0f, true);
    late_key.tracks[0].translation.push_back({2.5f, glm::vec3(3.0f)});
    CHECK(validate_clip(late_key, skeleton).has_value());

    AnimationClip no_time = walking_clip(0.0f, true);
    CHECK(validate_clip(no_time, skeleton).has_value());

    AnimationClip bent = walking_clip(2.0f, true);
    bent.tracks[0].rotation[1].value = glm::quat(2.0f, 0.0f, 0.0f, 0.0f);  // not unit length
    CHECK(validate_clip(bent, skeleton).has_value());
}

TEST_CASE("The reference pose is the bind pose at every time, and a clip with no keys holds it", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    AnimationClip empty = walking_clip(2.0f, true);
    empty.tracks = {JointTrack{}, JointTrack{}};
    for (const float time : {0.0f, 0.37f, 1.9f, 7.5f}) {
        const std::vector<JointPose> pose = sample_pose(skeleton, empty, time);
        CHECK(pose[1].translation == skeleton.bind_pose[1].translation);
        CHECK(pose[1].rotation == skeleton.bind_pose[1].rotation);
        CHECK(pose[1].scale == skeleton.bind_pose[1].scale);
    }

    // With keys, the first key at time zero is the reference: the bind pose of the skeleton, with the root at its first key.
    const std::vector<JointPose> start = sample_pose(skeleton, walking_clip(2.0f, true), 0.0f);
    CHECK(start[0].translation == glm::vec3(0.0f));
    CHECK(start[1].translation == skeleton.bind_pose[1].translation);
    const std::vector<glm::mat4> palette = skinning_palette(skeleton, skeleton.bind_pose);
    for (const glm::mat4& matrix : palette) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                CHECK(std::abs(matrix[c][r] - (c == r ? 1.0f : 0.0f)) < 1e-5f);
            }
        }
    }
}

TEST_CASE("Keys interpolate linearly for translation and by slerp for rotation, and hold outside a non-looping clip", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    AnimationClip clip = walking_clip(2.0f, false);
    // At t = 0.25 the root is a quarter of the way from x = 0 (at t = 0) to x = 1 (at t = 1). Its yaw is slerped over the 2 s clip to 90 degrees,
    // so it is an eighth of the way there: 90 / 8 degrees, which is pi / 16.
    const std::vector<JointPose> mid = sample_pose(skeleton, clip, 0.25f);
    CHECK_THAT(mid[0].translation.x, WithinAbs(0.25f, 1e-5f));
    CHECK_THAT(heading(mid[0].rotation), WithinAbs(kPi * 0.0625f, 1e-4f));

    // A non-looping clip holds its first key before zero and its last key after the end.
    CHECK_THAT(sample_pose(skeleton, clip, -5.0f)[0].translation.x, WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(sample_pose(skeleton, clip, 9.0f)[0].translation.x, WithinAbs(2.0f, 1e-6f));
    CHECK_THAT(wrap_time(clip, 9.0f), WithinAbs(2.0f, 1e-6f));
}

TEST_CASE("A looping clip is continuous through its loop seam, and the pose at the seam matches the start", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    // Keys at 0, 0.5 and 1.5 for the root's x: 0, 1, and 1.5. The seam runs from 1.5 back to 0 at time 2, so the value at 1.75 is halfway
    // between 1.5 and 0, that is 0.75.
    AnimationClip clip = walking_clip(2.0f, true);
    clip.tracks[0].translation = {{0.0f, glm::vec3(0.0f)}, {1.0f, glm::vec3(1.0f, 0.0f, 0.0f)}, {1.5f, glm::vec3(1.5f, 0.0f, 0.0f)}};
    CHECK_THAT(sample_pose(skeleton, clip, 1.75f)[0].translation.x, WithinAbs(0.75f, 1e-5f));

    // Sampled densely across two cycles, the pose never jumps: consecutive samples differ by at most the steepest slope times the step. The
    // steepest piece is the seam itself, which falls 1.5 m in 0.5 s, so 3 m/s. A jump would be far larger than that.
    const float step = 1.0f / 1000.0f;
    std::vector<JointPose> previous = sample_pose(skeleton, clip, 0.0f);
    float worst = 0.0f;
    for (int i = 1; i <= 4000; ++i) {
        const std::vector<JointPose> current = sample_pose(skeleton, clip, static_cast<float>(i) * step);
        worst = std::max(worst, glm::length(current[0].translation - previous[0].translation));
        previous = current;
    }
    CHECK(worst <= 3.5f * step);

    // The pose one period after a time equals the pose at that time, for both channels.
    const std::vector<JointPose> a = sample_pose(skeleton, clip, 0.3f);
    const std::vector<JointPose> b = sample_pose(skeleton, clip, 0.3f + clip.duration);
    CHECK(glm::length(a[0].translation - b[0].translation) < 1e-5f);
    CHECK(glm::length(a[0].translation - sample_pose(skeleton, clip, 0.3f - 2.0f * clip.duration)[0].translation) < 1e-5f);
}

TEST_CASE("The player advances with speed, wraps through loops and counts cycles, and a non-looping clip finishes", "[unit]") {
    const AnimationClip looping = walking_clip(2.0f, true);
    ClipPlayer player(looping, 2.0f);
    player.advance(1.5f);  // 3 s of clip time: one full cycle and 1 s more
    CHECK_THAT(player.elapsed(), WithinAbs(3.0f, 1e-5f));
    CHECK_THAT(player.time(), WithinAbs(1.0f, 1e-5f));
    CHECK(player.cycles() == 1);
    CHECK_FALSE(player.finished());

    player.set_speed(-1.0f);
    player.advance(4.0f);  // from 3 s back to -1 s: wrapped into the previous cycle
    CHECK(player.cycles() == -1);
    CHECK_THAT(player.time(), WithinAbs(1.0f, 1e-5f));

    const AnimationClip once = walking_clip(2.0f, false);
    ClipPlayer one_shot(once);
    one_shot.advance(5.0f);
    CHECK_THAT(one_shot.elapsed(), WithinAbs(2.0f, 1e-6f));
    CHECK(one_shot.finished());
    CHECK(one_shot.cycles() == 0);
}

TEST_CASE("Root motion reports the root's displacement over whole cycles, and the pose plus the displacement rebuilds the authored root", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    const AnimationClip clip = walking_clip(2.0f, true);
    RootMotion root;
    root.joint = 0;
    root.translate_x = true;
    root.translate_z = false;
    root.yaw = true;

    // Half a cycle: the root has moved 1 m in x, and turned 45 degrees.
    const RootDisplacement half = root_displacement(skeleton, clip, root, 1.0f);
    CHECK_THAT(half.translation.x, WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(half.yaw, WithinAbs(kPi * 0.25f, 1e-4f));

    // One and a half cycles: the first cycle adds 2 m and 90 degrees, and the root is 1 m along in the second.
    const RootDisplacement later = root_displacement(skeleton, clip, root, 3.0f);
    CHECK_THAT(later.translation.x, WithinAbs(3.0f, 1e-4f));
    CHECK_THAT(later.yaw, WithinAbs(kPi * 0.25f + kPi * 0.5f, 1e-4f));

    // The displacement of the pose is stripped out, and the root's pose stays at its start on the extracted axes.
    std::vector<JointPose> pose = sample_pose(skeleton, clip, 1.0f);
    const float authored_x = pose[0].translation.x;
    remove_root_motion(skeleton, clip, root, pose, 1.0f);
    CHECK_THAT(pose[0].translation.x, WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(heading(pose[0].rotation), WithinAbs(0.0f, 1e-4f));
    // Rebuilt: pose plus displacement is the authored root (x = 1 at t = 1 s in the first cycle).
    CHECK_THAT(pose[0].translation.x + half.translation.x, WithinAbs(authored_x, 1e-4f));
    CHECK_THAT(authored_x, WithinAbs(1.0f, 1e-5f));
    // The rebuilt yaw: the entity's yaw times the pose's yaw is the authored yaw.
    CHECK_THAT(heading(glm::angleAxis(half.yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * pose[0].rotation), WithinAbs(kPi * 0.25f, 1e-4f));

    // Over four cycles, sampled every 20 ms, the displacement never jumps: the largest step is the fastest motion (2 m/s) times the step. It ends
    // at 8 m, four cycles of 2 m each.
    float previous = 0.0f;
    float largest_step = 0.0f;
    for (int i = 1; i <= 400; ++i) {
        const float t = 2.0f * static_cast<float>(i) / 100.0f;
        const float x = root_displacement(skeleton, clip, root, t).translation.x;
        largest_step = std::max(largest_step, std::abs(x - previous));
        previous = x;
    }
    CHECK(largest_step <= 2.0f * 0.02f + 1e-4f);
    CHECK_THAT(previous, WithinAbs(8.0f, 1e-3f));
}

TEST_CASE("A blend of two clips is the weighted pose at each end, and the root displacement blends the same way", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    AnimationClip idle = walking_clip(2.0f, true);
    idle.tracks[0].translation = {{0.0f, glm::vec3(0.0f)}};  // the root stays put
    idle.tracks[0].rotation.clear();
    const AnimationClip walk = walking_clip(2.0f, true);
    RootMotion root;
    root.joint = 0;

    const BlendSample at_a = sample_blend(skeleton, idle, 0.5f, walk, 0.5f, 0.0f, root);
    const BlendSample at_b = sample_blend(skeleton, idle, 0.5f, walk, 0.5f, 1.0f, root);
    CHECK_THAT(at_a.root.translation.x, WithinAbs(0.0f, 1e-5f));
    CHECK_THAT(at_b.root.translation.x, WithinAbs(root_displacement(skeleton, walk, root, 0.5f).translation.x, 1e-5f));
    const BlendSample half = sample_blend(skeleton, idle, 0.5f, walk, 0.5f, 0.5f, root);
    CHECK_THAT(half.root.translation.x, WithinAbs(0.5f * at_b.root.translation.x, 1e-5f));
    CHECK(half.pose.size() == 2);
    CHECK_THAT(half.pose[1].translation.y, WithinAbs(skeleton.bind_pose[1].translation.y, 1e-6f));
}

TEST_CASE("Animated bounds cover a skinned vertex at every sampled pose, and the bind pose alone is not enough", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    AnimationClip clip = walking_clip(2.0f, true);
    clip.tracks[0].translation = {{0.0f, glm::vec3(-1.0f, 0.0f, 0.0f)}, {1.0f, glm::vec3(1.0f, 0.0f, 0.0f)}};
    clip.tracks[0].rotation.clear();
    SkinVertices skin;
    skin.positions = {glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    skin.joints = {glm::uvec4(0), glm::uvec4(0)};
    skin.weights = {glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.5f, 0.5f, 0.0f, 0.0f)};
    REQUIRE_FALSE(validate_skin(skin, skeleton).has_value());

    const BoundsSphere sphere = animated_bounds(skeleton, clip, skin);
    // The root moves the first vertex from x = -1 to x = 1, so both ends must lie in the sphere.
    CHECK(glm::length(glm::vec3(-1.0f, 0.0f, 0.0f) - sphere.center) <= sphere.radius);
    CHECK(glm::length(glm::vec3(1.0f, 0.0f, 0.0f) - sphere.center) <= sphere.radius);
    // The bind-pose sphere is smaller: it misses the posed extremes.
    const BoundsSphere bind_only = animated_bounds(skeleton, AnimationClip{"hold", 1.0f, true, {JointTrack{}, JointTrack{}}}, skin);
    CHECK(bind_only.radius < sphere.radius);
}

TEST_CASE("Skin data is checked: weights sum to one, and every weighted joint exists in the skeleton", "[unit]") {
    const Skeleton skeleton = two_joint_skeleton();
    SkinVertices skin;
    skin.positions = {glm::vec3(0.0f)};
    skin.joints = {glm::uvec4(0, 1, 0, 0)};
    skin.weights = {glm::vec4(0.5f, 0.5f, 0.0f, 0.0f)};
    CHECK_FALSE(validate_skin(skin, skeleton).has_value());

    SkinVertices unnormalized = skin;
    unnormalized.weights = {glm::vec4(0.5f, 0.2f, 0.0f, 0.0f)};
    CHECK(validate_skin(unnormalized, skeleton).has_value());

    SkinVertices missing_joint = skin;
    missing_joint.joints = {glm::uvec4(0, 7, 0, 0)};
    CHECK(validate_skin(missing_joint, skeleton).has_value());

    glm::vec4 empty(0.0f);
    normalize_weights(empty);
    CHECK(empty == glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
}
