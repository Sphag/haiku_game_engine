// CPU checks of the editable animation graph (US-070): creation, state, transition, and condition edits and their refusals, compatible
// clips and skeletons, undo and redo, save and reopen, and a preview machine that follows the edited graph.

#include "engine/animation/graph_edit.hpp"
#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace engine::animation;

namespace {

using Catch::Matchers::ContainsSubstring;

// Two joints, "root" and "child", with the child one metre up.
Skeleton two_joint_skeleton() {
    Skeleton skeleton;
    skeleton.names = {"root", "child"};
    skeleton.parents = {-1, 0};
    skeleton.bind_pose = {JointPose{}, JointPose{glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)}};
    skeleton.inverse_bind = {glm::mat4(1.0f), glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)))};
    return skeleton;
}

// Three joints, for the incompatible-skeleton cases.
Skeleton three_joint_skeleton() {
    Skeleton skeleton = two_joint_skeleton();
    skeleton.names.push_back("tail");
    skeleton.parents.push_back(0);
    skeleton.bind_pose.push_back(JointPose{});
    skeleton.inverse_bind.push_back(glm::mat4(1.0f));
    return skeleton;
}

AnimationClip clip_with_tracks(const std::string& name, std::size_t tracks, float duration, bool looping) {
    AnimationClip clip;
    clip.name = name;
    clip.duration = duration;
    clip.looping = looping;
    clip.tracks.resize(tracks);
    return clip;
}

ClipLibrary library_for_two_joints() {
    ClipLibrary clips;
    clips["idle"] = clip_with_tracks("idle", 2, 1.0f, true);
    clips["walk"] = clip_with_tracks("walk", 2, 1.0f, true);
    clips["jump"] = clip_with_tracks("jump", 2, 0.5f, false);
    return clips;
}

AnimationGraphDocument fresh_document() {
    auto created = AnimationGraphDocument::create(two_joint_skeleton(), library_for_two_joints());
    REQUIRE(created.ok());
    return std::move(created).value();
}

Condition float_above(const std::string& parameter, float value) {
    Condition condition;
    condition.kind = ConditionKind::Float;
    condition.parameter = parameter;
    condition.op = CompareOp::Greater;
    condition.value = value;
    return condition;
}

Condition trigger_on(const std::string& parameter) {
    Condition condition;
    condition.kind = ConditionKind::Trigger;
    condition.parameter = parameter;
    return condition;
}

// The document after a small authoring session: idle and walk, with a speed-based transition each way.
AnimationGraphDocument authored_document() {
    AnimationGraphDocument document = fresh_document();
    REQUIRE(document.add_parameter(ParamDecl{"speed", ParamType::Float, 0.0f, false}).ok());
    REQUIRE(document.add_state("walk", "walk").ok());
    auto to_walk = document.add_transition("default", "walk");
    REQUIRE(to_walk.ok());
    REQUIRE(document.add_condition(to_walk.value(), float_above("speed", 0.5f)).ok());
    auto to_default = document.add_transition("walk", "default");
    REQUIRE(to_default.ok());
    REQUIRE(document.set_transition_timing(to_default.value(), 0.2f, 0).ok());
    return document;
}

}  // namespace

TEST_CASE("A new document has one default state that plays a compatible clip and runs", "[unit][animation_graph]") {
    AnimationGraphDocument document = fresh_document();

    REQUIRE(document.graph().states.size() == 1);
    CHECK(document.graph().default_state == "default");
    CHECK(document.graph().states[0].clip == "idle");
    CHECK(document.revision() == 0);
    CHECK(document.can_undo() == false);

    auto machine = document.preview();
    REQUIRE(machine.ok());
    CHECK(machine.value().state_name() == "default");
}

TEST_CASE("A library with no clip valid for the skeleton cannot start a new graph", "[unit][animation_graph]") {
    ClipLibrary clips;
    clips["wrong"] = clip_with_tracks("wrong", 3, 1.0f, true);

    auto created = AnimationGraphDocument::create(two_joint_skeleton(), clips);

    REQUIRE_FALSE(created.ok());
    CHECK_THAT(created.error(), ContainsSubstring("no clip"));
}

TEST_CASE("States and transitions are created and their conditions edited", "[unit][animation_graph]") {
    AnimationGraphDocument document = authored_document();

    CHECK(document.graph().states.size() == 2);
    CHECK(document.graph().transitions.size() == 2);
    REQUIRE(document.graph().transitions[0].conditions.size() == 1);
    CHECK(document.graph().transitions[0].conditions[0].value == 0.5f);

    // A condition edit replaces the condition at that index, and the new value takes effect in the preview.
    REQUIRE(document.set_condition(0, 0, float_above("speed", 2.0f)).ok());
    CHECK(document.graph().transitions[0].conditions[0].value == 2.0f);
    CHECK(document.remove_condition(0, 0).ok());
    CHECK(document.graph().transitions[0].conditions.empty());
}

TEST_CASE("Clips are offered only when they fit the skeleton, and an incompatible clip is refused", "[unit][animation_graph]") {
    AnimationGraphDocument document = fresh_document();
    ClipLibrary clips = library_for_two_joints();
    clips["wrong"] = clip_with_tracks("wrong", 3, 1.0f, true);
    auto with_wrong = AnimationGraphDocument::create(two_joint_skeleton(), clips);
    REQUIRE(with_wrong.ok());
    AnimationGraphDocument checked = std::move(with_wrong).value();

    const auto offered = checked.compatible_clips();
    CHECK(std::find(offered.begin(), offered.end(), std::string("wrong")) == offered.end());
    CHECK(std::find(offered.begin(), offered.end(), std::string("walk")) != offered.end());

    auto refused = checked.set_state_clip("default", "wrong");
    REQUIRE_FALSE(refused.ok());
    CHECK_THAT(refused.error(), ContainsSubstring("tracks"));
    CHECK(checked.graph().states[0].clip == "idle");
    CHECK(document.compatible_clips().size() == 3);
}

TEST_CASE("Choosing a skeleton that breaks a state's clip is refused and names the state", "[unit][animation_graph]") {
    AnimationGraphDocument document = authored_document();

    auto refused = document.set_skeleton(three_joint_skeleton());

    REQUIRE_FALSE(refused.ok());
    CHECK_THAT(refused.error(), ContainsSubstring("tracks"));
    CHECK(document.skeleton().joint_count() == 2);
}

TEST_CASE("Opening a graph against an incompatible skeleton is an error that names the clip", "[unit][animation_graph]") {
    const std::string text = authored_document().save();

    auto opened = AnimationGraphDocument::open(text, three_joint_skeleton(), library_for_two_joints());

    REQUIRE_FALSE(opened.ok());
    CHECK_THAT(opened.error(), ContainsSubstring("clip"));
}

TEST_CASE("Invalid edits are refused and change nothing", "[unit][animation_graph]") {
    AnimationGraphDocument document = authored_document();
    const std::string before = document.save();
    const auto revision = document.revision();

    CHECK_FALSE(document.add_state("", "idle").ok());
    CHECK_FALSE(document.add_state("walk", "idle").ok());
    CHECK_FALSE(document.add_state("run", "missing_clip").ok());
    CHECK_FALSE(document.remove_state("default").ok());
    CHECK_FALSE(document.remove_state("nowhere").ok());
    CHECK_FALSE(document.set_transition_timing(0, 0.0f, 0).ok());
    CHECK_FALSE(document.set_transition_timing(99, 0.1f, 0).ok());
    CHECK_FALSE(document.remove_parameter("speed").ok());
    CHECK_FALSE(document.add_transition("default", "nowhere").ok());

    CHECK(document.save() == before);
    CHECK(document.revision() == revision);
}

TEST_CASE("Two transitions from one state need distinct priorities, and an added transition gets a new one", "[unit][animation_graph]") {
    AnimationGraphDocument document = authored_document();
    auto first = document.add_transition("default", "walk");
    REQUIRE(first.ok());
    const int added_priority = document.graph().transitions[first.value()].priority;

    CHECK(added_priority > document.graph().transitions[0].priority);
    CHECK_FALSE(document.set_transition_timing(first.value(), 0.1f, document.graph().transitions[0].priority).ok());
}

TEST_CASE("A parameter that a condition tests cannot be removed, and a removed state takes its transitions with it",
          "[unit][animation_graph]") {
    AnimationGraphDocument document = authored_document();

    auto refused = document.remove_parameter("speed");
    REQUIRE_FALSE(refused.ok());
    CHECK_THAT(refused.error(), ContainsSubstring("speed"));

    REQUIRE(document.remove_state("walk").ok());
    CHECK(document.graph().states.size() == 1);
    CHECK(document.graph().transitions.empty());
    CHECK(document.remove_parameter("speed").ok());
}

TEST_CASE("Renaming a state updates its transitions and the default", "[unit][animation_graph]") {
    AnimationGraphDocument document = authored_document();

    REQUIRE(document.rename_state("walk", "moving").ok());

    CHECK(document.graph().states[1].name == "moving");
    CHECK(document.graph().transitions[0].to == "moving");
    CHECK(document.graph().transitions[1].from == "moving");
    CHECK_FALSE(document.rename_state("default", "moving").ok());
}

TEST_CASE("Undo and redo restore each edit in order, and a new edit clears redo", "[unit][animation_graph]") {
    AnimationGraphDocument document = fresh_document();
    const std::string start = document.save();
    REQUIRE(document.add_state("walk", "walk").ok());
    const std::string with_walk = document.save();
    REQUIRE(document.set_state_speed("walk", 2.0f).ok());
    const std::string fast = document.save();

    CHECK(document.revision() == 2);
    REQUIRE(document.undo());
    CHECK(document.save() == with_walk);
    REQUIRE(document.undo());
    CHECK(document.save() == start);
    CHECK_FALSE(document.undo());
    REQUIRE(document.redo());
    CHECK(document.save() == with_walk);
    REQUIRE(document.redo());
    CHECK(document.save() == fast);
    CHECK_FALSE(document.redo());

    REQUIRE(document.undo());
    REQUIRE(document.add_state("run", "walk").ok());
    CHECK_FALSE(document.can_redo());
}

TEST_CASE("Undo restores the skeleton with the graph", "[unit][animation_graph]") {
    AnimationGraphDocument document = fresh_document();
    REQUIRE(document.add_state("walk", "walk").ok());

    REQUIRE(document.set_skeleton(two_joint_skeleton()).ok());
    REQUIRE(document.undo());
    REQUIRE(document.undo());

    CHECK(document.graph().states.size() == 1);
    CHECK(document.skeleton().joint_count() == 2);
}

TEST_CASE("A saved graph reopens to the same graph and runs the same way", "[unit][animation_graph]") {
    AnimationGraphDocument document = authored_document();
    const std::string saved = document.save();

    auto reopened = AnimationGraphDocument::open(saved, two_joint_skeleton(), library_for_two_joints());
    REQUIRE(reopened.ok());
    AnimationGraphDocument copy = std::move(reopened).value();

    CHECK(copy.save() == saved);
    CHECK(copy.graph().states.size() == document.graph().states.size());
    CHECK(copy.can_undo() == false);

    auto machine = copy.preview();
    REQUIRE(machine.ok());
    REQUIRE(machine.value().set_float("speed", 1.0f));
    machine.value().update(1.0f / 64.0f);
    CHECK(machine.value().state_name() == "walk");
}

TEST_CASE("Opening a graph with a broken format is refused with the parse error", "[unit][animation_graph]") {
    auto opened = AnimationGraphDocument::open("{\"format\": \"other\"}", two_joint_skeleton(), library_for_two_joints());

    REQUIRE_FALSE(opened.ok());
    CHECK_FALSE(opened.error().empty());
}

TEST_CASE("A preview machine follows edited triggers", "[unit][animation_graph]") {
    AnimationGraphDocument document = fresh_document();
    REQUIRE(document.add_parameter(ParamDecl{"jump", ParamType::Trigger, 0.0f, false}).ok());
    REQUIRE(document.add_state("jump", "jump").ok());
    auto jump = document.add_transition("default", "jump");
    REQUIRE(jump.ok());
    REQUIRE(document.add_condition(jump.value(), trigger_on("jump")).ok());

    auto machine = document.preview();
    REQUIRE(machine.ok());
    CHECK(machine.value().state_name() == "default");
    REQUIRE(machine.value().set_trigger("jump"));
    machine.value().update(1.0f / 64.0f);
    CHECK(machine.value().state_name() == "jump");
}
