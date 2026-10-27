// The animation module of the Lua API (US-069). Scripts change the parameters of the named state machines in the bound AnimatorSet, and
// read their state. The host steps the set, so a script's change takes effect at the next update, as it does in a game. The tests check
// the transitions a script's changes cause, the controlled errors for a missing machine, a wrong parameter, and a bad argument, and that
// the Lua stack is empty after every call. They run on every operating system with no renderer.

#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/script/script_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <utility>

using engine::animation::AnimationClip;
using engine::animation::AnimatorSet;
using engine::animation::ClipLibrary;
using engine::animation::JointPose;
using engine::animation::Skeleton;
using engine::animation::StateGraph;
using engine::animation::StateMachine;
using engine::animation::parse_state_graph;
using engine::script::ScriptEngine;
using engine::script::ScriptError;
using engine::script::ScriptErrorKind;
using engine::script::ScriptServices;

namespace {

constexpr float kStep = 1.0f / 64.0f;

// Idle is the default. A positive speed walks; a trigger jumps. A single joint and two clips are enough for these checks.
constexpr const char* kGraphJson = R"({
  "format": "engine.animation_graph", "version": 1,
  "parameters": [
    {"name": "speed", "type": "float", "default": 0},
    {"name": "grounded", "type": "bool", "default": true},
    {"name": "jump", "type": "trigger"}
  ],
  "default_state": "idle",
  "states": [
    {"name": "idle", "clip": "idle", "speed": 1},
    {"name": "walk", "clip": "walk", "speed": 1},
    {"name": "jump", "clip": "idle", "speed": 1}
  ],
  "transitions": [
    {"from": "idle", "to": "walk", "duration": 0.25, "priority": 1,
     "conditions": [{"when": "float", "parameter": "speed", "op": ">", "value": 0.1}]},
    {"from": "idle", "to": "jump", "duration": 0.25, "priority": 5,
     "conditions": [{"when": "trigger", "parameter": "jump"}]}
  ]
})";

// A machine on a one-joint skeleton. Both clips are one second long with no keys, so the pose is the bind pose throughout.
StateMachine make_machine() {
    Skeleton skeleton;
    skeleton.names = {"root"};
    skeleton.parents = {-1};
    skeleton.bind_pose = {JointPose{}};
    skeleton.inverse_bind = {glm::mat4(1.0f)};

    AnimationClip clip;
    clip.duration = 1.0f;
    clip.looping = true;
    clip.tracks.resize(1);
    ClipLibrary clips;
    clip.name = "idle";
    clips.emplace("idle", clip);
    clip.name = "walk";
    clips.emplace("walk", clip);

    auto parsed = parse_state_graph(kGraphJson);
    REQUIRE(parsed.ok());
    auto created = StateMachine::create(parsed.value(), skeleton, clips);
    REQUIRE(created.ok());
    return std::move(created).value();
}

// Runs a chunk that must succeed, and leaves the stack empty.
void run_ok(ScriptEngine& engine, const char* source) {
    const auto result = engine.run("test", source);
    const std::string detail = result.ok() ? std::string() : result.error().message;
    INFO(detail);
    REQUIRE(result.ok());
    CHECK(engine.stack_depth() == 0);
}

// Runs a chunk that must fail with the given kind, and whose message contains `needle`.
void run_error(ScriptEngine& engine, const char* source, ScriptErrorKind kind, const char* needle) {
    const auto result = engine.run("test", source);
    REQUIRE_FALSE(result.ok());
    const ScriptError error = result.error();
    INFO(error.message);
    CHECK(error.kind == kind);
    CHECK(error.message.find(needle) != std::string::npos);
    CHECK(engine.stack_depth() == 0);
}

}  // namespace

TEST_CASE("A script's parameter change takes a transition at the host's next update, and the script reads the new state", "[unit]") {
    AnimatorSet animators;
    REQUIRE(animators.add("hero", make_machine()));
    ScriptEngine engine;
    ScriptServices services;
    services.animators = &animators;
    engine.bind(services);

    run_ok(engine, "assert(animation.state('hero') == 'idle') assert(not animation.blending('hero'))");
    run_ok(engine, "animation.set_float('hero', 'speed', 1.0)");
    CHECK(animators.find("hero")->get_float("speed") == std::optional<float>(1.0f));
    CHECK(animators.find("hero")->state_name() == "idle");  // nothing changes until the host steps the set

    animators.update(kStep);
    run_ok(engine, "assert(animation.state('hero') == 'walk') assert(animation.blending('hero'))");
    run_ok(engine, "assert(animation.get_float('hero', 'speed') == 1.0)");
}

TEST_CASE("A script's trigger starts the higher-priority transition, and the trigger is consumed by it", "[unit]") {
    AnimatorSet animators;
    REQUIRE(animators.add("hero", make_machine()));
    ScriptEngine engine;
    ScriptServices services;
    services.animators = &animators;
    engine.bind(services);

    run_ok(engine, "animation.set_float('hero', 'speed', 1.0) animation.trigger('hero', 'jump')");
    animators.update(kStep);
    run_ok(engine, "assert(animation.state('hero') == 'jump') assert(animation.get_trigger('hero', 'jump') == false)");
}

TEST_CASE("A bool parameter is set from Lua and read back", "[unit]") {
    AnimatorSet animators;
    REQUIRE(animators.add("hero", make_machine()));
    ScriptEngine engine;
    ScriptServices services;
    services.animators = &animators;
    engine.bind(services);

    run_ok(engine, "animation.set_bool('hero', 'grounded', false) assert(animation.get_bool('hero', 'grounded') == false)");
    CHECK(animators.find("hero")->get_bool("grounded") == std::optional<bool>(false));
}

TEST_CASE("Unknown machines, parameters of the wrong type, and bad arguments raise controlled errors and change nothing", "[unit]") {
    AnimatorSet animators;
    REQUIRE(animators.add("hero", make_machine()));
    ScriptEngine engine;
    ScriptServices services;
    services.animators = &animators;
    engine.bind(services);

    run_error(engine, "animation.set_float('nobody', 'speed', 1.0)", ScriptErrorKind::Runtime, "no animator named 'nobody'");
    run_error(engine, "animation.set_bool('hero', 'speed', true)", ScriptErrorKind::Runtime, "has no bool parameter 'speed'");
    run_error(engine, "animation.set_float('hero', 'grounded', 1.0)", ScriptErrorKind::Runtime, "has no float parameter 'grounded'");
    run_error(engine, "animation.trigger('hero', 'speed')", ScriptErrorKind::Runtime, "has no trigger parameter 'speed'");
    run_error(engine, "animation.set_float('hero', 'speed', 0 / 0)", ScriptErrorKind::Runtime, "value must be a finite number");
    run_error(engine, "animation.set_bool('hero', 'grounded', 1)", ScriptErrorKind::Runtime, "boolean expected");

    CHECK(animators.find("hero")->get_float("speed") == std::optional<float>(0.0f));
    CHECK(animators.find("hero")->get_bool("grounded") == std::optional<bool>(true));
}

TEST_CASE("Without a bound animator set, the animation module raises a controlled error", "[unit]") {
    ScriptEngine engine;
    run_error(engine, "animation.state('hero')", ScriptErrorKind::Runtime, "no animator set is bound");
}
