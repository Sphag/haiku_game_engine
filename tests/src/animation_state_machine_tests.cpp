// Animation state machines (US-069), on the CPU. The fixtures step with a fixed 1/64 s, which every sum in them represents exactly, so the
// counts of steps, loop events, and blend weights are exact. The expected poses come from the clip definitions in closed form. The tests
// cover the graph format (round trip, version, and validation), creation errors, entry and the default state, float/bool/trigger
// conditions, priority, trigger consumption, timed blends, loop and finish events, and root motion through a blend.

#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace engine::animation;

namespace {

using Catch::Matchers::WithinAbs;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kStep = 1.0f / 64.0f;

// Two joints: "root" at the origin, and "child" one metre up. The child is the joint whose pose the fixtures check.
Skeleton two_joint_skeleton() {
    Skeleton skeleton;
    skeleton.names = {"root", "child"};
    skeleton.parents = {-1, 0};
    skeleton.bind_pose = {JointPose{}, JointPose{glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)}};
    skeleton.inverse_bind = {glm::inverse(glm::mat4(1.0f)), glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)))};
    return skeleton;
}

// 1 s, looping. The root walks 1 m along +x and turns 90 degrees about +Y over the cycle, and the child bobs from 1.0 up to 1.25 and back.
AnimationClip walk_clip() {
    AnimationClip clip;
    clip.name = "walk";
    clip.duration = 1.0f;
    clip.looping = true;
    clip.tracks.resize(2);
    clip.tracks[0].translation = {{0.0f, glm::vec3(0.0f)}, {1.0f, glm::vec3(1.0f, 0.0f, 0.0f)}};
    clip.tracks[0].rotation = {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}, {1.0f, glm::angleAxis(kPi * 0.5f, glm::vec3(0.0f, 1.0f, 0.0f))}};
    clip.tracks[1].translation = {{0.0f, glm::vec3(0.0f, 1.0f, 0.0f)},
                                  {0.5f, glm::vec3(0.0f, 1.25f, 0.0f)},
                                  {1.0f, glm::vec3(0.0f, 1.0f, 0.0f)}};
    return clip;
}

// 1 s, looping, with no keys: the bind pose at every time and no root motion.
AnimationClip idle_clip() {
    AnimationClip clip;
    clip.name = "idle";
    clip.duration = 1.0f;
    clip.looping = true;
    clip.tracks.resize(2);
    return clip;
}

// 0.5 s, not looping. The child rises from 1 to 2 metres.
AnimationClip jump_clip() {
    AnimationClip clip;
    clip.name = "jump";
    clip.duration = 0.5f;
    clip.looping = false;
    clip.tracks.resize(2);
    clip.tracks[1].translation = {{0.0f, glm::vec3(0.0f, 1.0f, 0.0f)}, {0.5f, glm::vec3(0.0f, 2.0f, 0.0f)}};
    return clip;
}

ClipLibrary library() {
    ClipLibrary clips;
    clips.emplace("walk", walk_clip());
    clips.emplace("idle", idle_clip());
    clips.emplace("jump", jump_clip());
    return clips;
}

// idle (default) -> walk while speed > 0.1; walk -> idle while speed <= 0.1; idle -> jump on the trigger (priority 5, beats walk);
// jump -> land when the jump clip has finished. Parameters: speed (float, 0), grounded (bool, true), jump (trigger).
constexpr const char* kGraphJson = R"({
  "format": "engine.animation_graph", "version": 1,
  "parameters": [
    {"name": "speed", "type": "float", "default": 0},
    {"name": "grounded", "type": "bool", "default": true},
    {"name": "jump", "type": "trigger"}
  ],
  "default_state": "idle",
  "root": {"joint": "root", "translate_x": true, "translate_y": false, "translate_z": true, "yaw": true},
  "states": [
    {"name": "idle", "clip": "idle", "speed": 1},
    {"name": "walk", "clip": "walk", "speed": 1},
    {"name": "jump", "clip": "jump", "speed": 1},
    {"name": "land", "clip": "idle", "speed": 1}
  ],
  "transitions": [
    {"from": "idle", "to": "walk", "duration": 0.25, "priority": 1,
     "conditions": [{"when": "float", "parameter": "speed", "op": ">", "value": 0.1}]},
    {"from": "walk", "to": "idle", "duration": 0.25, "priority": 1,
     "conditions": [{"when": "float", "parameter": "speed", "op": "<=", "value": 0.1}]},
    {"from": "idle", "to": "jump", "duration": 0.25, "priority": 5,
     "conditions": [{"when": "trigger", "parameter": "jump"}]},
    {"from": "jump", "to": "land", "duration": 0.25, "priority": 1,
     "conditions": [{"when": "finished"}]}
  ]
})";

StateGraph base_graph() {
    auto parsed = parse_state_graph(kGraphJson);
    REQUIRE(parsed.ok());
    return parsed.value();
}

StateMachine make_machine(const StateGraph& graph) {
    auto created = StateMachine::create(graph, two_joint_skeleton(), library());
    const std::string detail = created.ok() ? std::string() : created.error();
    INFO(detail);
    REQUIRE(created.ok());
    return std::move(created).value();
}

std::uint32_t index_of(const StateGraph& graph, const std::string& name) {
    for (std::size_t i = 0; i < graph.states.size(); ++i) {
        if (graph.states[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    REQUIRE(false);
    return 0;
}

std::vector<AnimationEvent> events_of(StateMachine& machine, AnimationEvent::Kind kind) {
    std::vector<AnimationEvent> out;
    for (const AnimationEvent& event : machine.take_events()) {
        if (event.kind == kind) {
            out.push_back(event);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("A graph round-trips through its JSON form, and a wrong format, version, or key is refused", "[unit]") {
    const StateGraph graph = base_graph();
    CHECK(graph.states.size() == 4);
    CHECK(graph.transitions.size() == 4);
    CHECK(graph.default_state == "idle");

    const std::string first = state_graph_json(graph);
    auto reparsed = parse_state_graph(first);
    REQUIRE(reparsed.ok());
    CHECK(state_graph_json(reparsed.value()) == first);

    auto wrong_version = parse_state_graph(R"({"format":"engine.animation_graph","version":2})");
    REQUIRE_FALSE(wrong_version.ok());
    CHECK(wrong_version.error().find("version 2") != std::string::npos);

    auto wrong_format = parse_state_graph(R"({"format":"engine.material_graph","version":1})");
    REQUIRE_FALSE(wrong_format.ok());
    CHECK(wrong_format.error().find("engine.animation_graph") != std::string::npos);

    auto not_json = parse_state_graph("{ states: ");
    REQUIRE_FALSE(not_json.ok());
    CHECK(not_json.error().find("not valid JSON") != std::string::npos);
}

TEST_CASE("Every transition needs an explicit priority and duration, and its priority is distinct among its source's transitions", "[unit]") {
    // A transition without a priority is not defaulted.
    std::string no_priority = kGraphJson;
    const std::string::size_type at = no_priority.find("\"priority\": 5");
    REQUIRE(at != std::string::npos);
    no_priority.replace(at, std::string("\"priority\": 5").size(), "\"unused\": 5");
    auto missing = parse_state_graph(no_priority);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().find("priority") != std::string::npos);

    StateGraph tie = base_graph();
    tie.transitions[0].priority = 5;  // idle -> walk now ties with idle -> jump
    auto tied = validate_state_graph(tie);
    REQUIRE(tied.has_value());
    CHECK(tied->find("shares priority 5") != std::string::npos);

    StateGraph no_duration = base_graph();
    no_duration.transitions[0].duration = 0.0f;
    CHECK(validate_state_graph(no_duration).has_value());
}

TEST_CASE("A condition must test a parameter of its own type, and a default state must exist", "[unit]") {
    StateGraph wrong_type = base_graph();
    wrong_type.transitions[0].conditions[0].kind = ConditionKind::Bool;  // speed is a float
    wrong_type.transitions[0].conditions[0].parameter = "speed";
    auto problem = validate_state_graph(wrong_type);
    REQUIRE(problem.has_value());
    CHECK(problem->find("as bool") != std::string::npos);

    StateGraph unknown = base_graph();
    unknown.transitions[0].conditions[0].parameter = "nope";
    CHECK(validate_state_graph(unknown).has_value());

    StateGraph no_default = base_graph();
    no_default.default_state = "crouch";
    CHECK(validate_state_graph(no_default).has_value());
}

TEST_CASE("Creation names what is wrong with the clip library and the root joint", "[unit]") {
    const StateGraph graph = base_graph();

    ClipLibrary missing = library();
    missing.erase("walk");
    auto no_clip = StateMachine::create(graph, two_joint_skeleton(), missing);
    REQUIRE_FALSE(no_clip.ok());
    CHECK(no_clip.error().find("\"walk\"") != std::string::npos);

    ClipLibrary bad = library();
    bad["walk"].tracks.resize(1);  // one track for a two-joint skeleton
    auto wrong_tracks = StateMachine::create(graph, two_joint_skeleton(), bad);
    REQUIRE_FALSE(wrong_tracks.ok());
    CHECK(wrong_tracks.error().find("clip \"walk\"") != std::string::npos);

    StateGraph other_root = graph;
    other_root.root.joint = "hips";
    auto no_root = StateMachine::create(other_root, two_joint_skeleton(), library());
    REQUIRE_FALSE(no_root.ok());
    CHECK(no_root.error().find("\"hips\"") != std::string::npos);
}

TEST_CASE("The machine enters its default state at time zero, and its first pose is the bind pose of a clip with no keys", "[unit]") {
    const StateGraph graph = base_graph();
    StateMachine machine = make_machine(graph);
    CHECK(machine.state_name() == "idle");
    CHECK_FALSE(machine.blending());
    CHECK(machine.blend_weight() == 1.0f);

    const std::vector<AnimationEvent> entered = machine.take_events();
    REQUIRE(entered.size() == 1);
    CHECK(entered[0].kind == AnimationEvent::Kind::Entered);
    CHECK(entered[0].state == index_of(graph, "idle"));

    REQUIRE(machine.pose().size() == 2);
    CHECK(machine.pose()[1].translation.y == 1.0f);
    CHECK(machine.root_delta().translation.x == 0.0f);
    CHECK(machine.root_delta().yaw == 0.0f);
}

TEST_CASE("Float, bool, and trigger parameters are set and read by name, and the wrong type or name is refused", "[unit]") {
    StateMachine machine = make_machine(base_graph());
    CHECK(machine.set_float("speed", 0.5f));
    CHECK(machine.get_float("speed") == std::optional<float>(0.5f));
    CHECK_FALSE(machine.set_float("grounded", 1.0f));
    CHECK_FALSE(machine.set_bool("speed", true));
    CHECK_FALSE(machine.set_float("missing", 1.0f));
    CHECK(machine.set_bool("grounded", false));
    CHECK(machine.get_bool("grounded") == std::optional<bool>(false));
    CHECK(machine.set_trigger("jump"));
    CHECK_FALSE(machine.set_trigger("speed"));
    CHECK(machine.get_trigger("jump") == std::optional<bool>(true));
    CHECK_FALSE(machine.get_float("jump").has_value());
}

TEST_CASE("A float condition starts a timed blend, the blend completes at its duration, and the pose is the entering clip at its time", "[unit]") {
    const StateGraph graph = base_graph();
    StateMachine machine = make_machine(graph);
    machine.take_events();
    REQUIRE(machine.set_float("speed", 1.0f));

    machine.update(kStep);  // the transition is taken in the first update, with the entering clip at time zero
    CHECK(machine.state_name() == "walk");
    CHECK(machine.source_state() == index_of(graph, "idle"));
    CHECK(machine.blending());
    CHECK(machine.blend_weight() == 0.0f);

    machine.update(kStep);
    CHECK(machine.blend_weight() == 1.0f / 16.0f);

    // The blend lasts 0.25 s = 16 steps. It completes in the 17th update, when the walk clip has run 16 steps.
    for (int step = 2; step < 16; ++step) {
        machine.update(kStep);
    }
    CHECK(machine.blending());
    CHECK(machine.blend_weight() == 15.0f / 16.0f);

    machine.update(kStep);
    CHECK_FALSE(machine.blending());
    CHECK(machine.state_name() == "walk");
    CHECK(machine.state_time() == 0.25f);
    // The child rises 0.25 m over the first half of the walk clip: 1 + 0.5 * t, with t = 0.25 s.
    REQUIRE(machine.pose().size() == 2);
    CHECK_THAT(machine.pose()[1].translation.y, WithinAbs(1.0f + 0.5f * 0.25f, 1e-6f));
}

TEST_CASE("Of the transitions whose conditions hold, the highest priority is taken, and a changed priority changes the choice", "[unit]") {
    // The trigger satisfies idle -> jump (priority 5), and speed satisfies idle -> walk (priority 1).
    StateGraph graph = base_graph();
    StateMachine low_walk = make_machine(graph);
    REQUIRE(low_walk.set_float("speed", 1.0f));
    REQUIRE(low_walk.set_trigger("jump"));
    low_walk.update(kStep);
    CHECK(low_walk.state_name() == "jump");
    CHECK(low_walk.get_trigger("jump") == std::optional<bool>(false));  // the trigger the taken transition tested is consumed

    graph.transitions[0].priority = 9;  // walk now beats jump
    StateMachine high_walk = make_machine(graph);
    REQUIRE(high_walk.set_float("speed", 1.0f));
    REQUIRE(high_walk.set_trigger("jump"));
    high_walk.update(kStep);
    CHECK(high_walk.state_name() == "walk");
    CHECK(high_walk.get_trigger("jump") == std::optional<bool>(true));  // not tested by the taken transition, so it stays set
}

TEST_CASE("A trigger set during a blend stays set until a transition that tests it is taken", "[unit]") {
    StateMachine machine = make_machine(base_graph());
    REQUIRE(machine.set_float("speed", 1.0f));
    machine.update(kStep);
    REQUIRE(machine.state_name() == "walk");
    REQUIRE(machine.set_trigger("jump"));

    // The walk state has no transition that tests the trigger, so the trigger is still set after the blend completes.
    for (int step = 0; step < 16; ++step) {
        machine.update(kStep);
    }
    CHECK_FALSE(machine.blending());
    CHECK(machine.state_name() == "walk");
    CHECK(machine.get_trigger("jump") == std::optional<bool>(true));
}

TEST_CASE("A looping clip reports one loop event per cycle boundary, and the cycle count is exact", "[unit]") {
    StateMachine machine = make_machine(base_graph());
    machine.take_events();  // the entry event
    for (int step = 0; step < 160; ++step) {  // 2.5 s of idle
        machine.update(kStep);
    }
    const std::vector<AnimationEvent> loops = events_of(machine, AnimationEvent::Kind::Loop);
    REQUIRE(loops.size() == 2);
    CHECK(loops[0].cycle == 1);
    CHECK(loops[1].cycle == 2);
    CHECK(loops[0].state == index_of(base_graph(), "idle"));
    CHECK(events_of(machine, AnimationEvent::Kind::Finished).empty());
}

TEST_CASE("A non-looping clip reports one finish event at its end, and a finished condition takes the transition out in that update", "[unit]") {
    const StateGraph graph = base_graph();
    StateMachine machine = make_machine(graph);
    machine.take_events();
    REQUIRE(machine.set_trigger("jump"));

    machine.update(kStep);  // idle -> jump; the jump clip starts at time zero
    CHECK(machine.state_name() == "jump");

    // The jump clip is 0.5 s = 32 steps long. It has run 31 steps after the 32nd update, and 32 steps after the 33rd.
    for (int step = 2; step <= 32; ++step) {
        machine.update(kStep);
    }
    CHECK(machine.state_name() == "jump");
    CHECK(events_of(machine, AnimationEvent::Kind::Finished).empty());

    machine.update(kStep);
    CHECK(machine.state_name() == "land");
    const std::vector<AnimationEvent> finished = events_of(machine, AnimationEvent::Kind::Finished);
    REQUIRE(finished.size() == 1);
    CHECK(finished[0].state == index_of(graph, "jump"));

    // The finish is reported once: more updates in the land state add none.
    for (int step = 0; step < 20; ++step) {
        machine.update(kStep);
    }
    CHECK(events_of(machine, AnimationEvent::Kind::Finished).empty());
}

TEST_CASE("Root motion over a whole walk cycle equals the authored root: 1 m of travel and a 90 degree turn per second", "[unit]") {
    StateGraph graph = base_graph();
    graph.parameters[0].default_float = 1.0f;  // speed starts above the walk threshold
    graph.default_state = "walk";
    StateMachine machine = make_machine(graph);

    float travelled = 0.0f;
    float turned = 0.0f;
    for (int step = 0; step < 64; ++step) {  // one second: one full cycle of the walk clip
        machine.update(kStep);
        travelled += machine.root_delta().translation.x;
        turned += machine.root_delta().yaw;
    }
    CHECK_THAT(travelled, WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(turned, WithinAbs(kPi * 0.5f, 1e-4f));
    CHECK(machine.root_delta().translation.y == 0.0f);
}

TEST_CASE("Root motion stays continuous through a blend, and its sum is the entering clip's root from the start of the blend", "[unit]") {
    StateGraph graph = base_graph();
    StateMachine machine = make_machine(graph);
    REQUIRE(machine.set_float("speed", 1.0f));

    // Idle has no root motion, so the blend moves the root from zero to the walk clip's motion with no step. A step would show as a
    // delta far above one update's walk, which is 1/64 m here.
    float largest = 0.0f;
    float total = 0.0f;
    for (int step = 0; step < 100; ++step) {
        machine.update(kStep);
        largest = std::max(largest, std::abs(machine.root_delta().translation.x));
        total += machine.root_delta().translation.x;
    }
    CHECK_FALSE(machine.blending());
    CHECK(machine.state_name() == "walk");
    CHECK(largest < 0.04f);
    CHECK_THAT(total, WithinAbs(machine.state_time(), 1e-4f));
}

TEST_CASE("The animator set holds machines by name and steps them together", "[unit]") {
    AnimatorSet set;
    CHECK(set.add("hero", make_machine(base_graph())));
    CHECK_FALSE(set.add("hero", make_machine(base_graph())));
    CHECK(set.size() == 1);

    StateMachine* hero = set.find("hero");
    REQUIRE(hero != nullptr);
    REQUIRE(hero->set_float("speed", 1.0f));
    set.update(kStep);
    CHECK(set.find("hero")->state_name() == "walk");

    CHECK(set.find("nobody") == nullptr);
    CHECK(set.remove("hero"));
    CHECK_FALSE(set.remove("hero"));
    CHECK(set.size() == 0);
}
