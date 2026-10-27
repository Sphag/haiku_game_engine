#pragma once

// Animation state graphs (US-069): the versioned, data-only description of a character's states, parameters, and transitions. The graph
// names clips and joints; it does not hold them. A StateMachine (state_machine.hpp) binds a graph to a skeleton and a clip library.
//
// Format (JSON, version 1). Every key below is required unless marked optional, and nothing is defaulted silently except the optional ones:
//   format        "engine.animation_graph"
//   version       1
//   parameters    [{name, type: "float"|"bool"|"trigger", default?}]   default is a number (float) or a boolean (bool)
//   default_state the state the machine enters when it is created
//   root          optional {joint, translate_x, translate_y, translate_z, yaw}; without it the graph has no root motion
//   states        [{name, clip, speed}]
//   transitions   [{from, to, duration, priority, conditions: [...]}]
// A condition is one of: {"when": "float", "parameter", "op": "<"|"<="|">"|">="|"=="|"!=", "value"}, {"when": "bool", "parameter", "is"},
// {"when": "trigger", "parameter"}, or {"when": "finished"} (the source state's non-looping clip has reached its end).
//
// Priority. Transitions leaving one state must carry distinct priorities. When several are satisfied in the same update, the highest
// priority wins. A tie is never resolved by file order, so the choice is always explicit.

#include "engine/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::animation {

inline constexpr std::string_view kGraphFormat = "engine.animation_graph";
inline constexpr std::uint32_t kGraphVersion = 1;

enum class ParamType : std::uint8_t { Float, Bool, Trigger };

enum class CompareOp : std::uint8_t { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual };

enum class ConditionKind : std::uint8_t { Float, Bool, Trigger, Finished };

struct ParamDecl {
    std::string name;
    ParamType type = ParamType::Float;
    float default_float = 0.0f;
    bool default_bool = false;
};

struct Condition {
    ConditionKind kind = ConditionKind::Finished;
    std::string parameter;              // Float, Bool, and Trigger
    CompareOp op = CompareOp::Greater;  // Float
    float value = 0.0f;                 // Float
    bool expected = true;               // Bool
};

struct StateDecl {
    std::string name;
    std::string clip;      // a clip name in the library the machine is created with
    float speed = 1.0f;    // finite; a negative speed plays the clip backward
};

struct TransitionDecl {
    std::string from;
    std::string to;
    float duration = 0.0f;  // seconds of blend, greater than zero
    int priority = 0;       // distinct among the transitions leaving `from`; higher wins
    std::vector<Condition> conditions;  // all must hold; an empty list always holds
};

// The root joint whose motion the entity takes (see playback.hpp). An empty joint name means the graph has no root motion.
struct RootDecl {
    std::string joint;
    bool translate_x = true;
    bool translate_y = false;
    bool translate_z = true;
    bool yaw = true;
};

struct StateGraph {
    std::vector<ParamDecl> parameters;
    std::vector<StateDecl> states;
    std::vector<TransitionDecl> transitions;
    std::string default_state;
    RootDecl root;
};

// Checks the graph's own rules: unique non-empty names, a default state that exists, transitions between known states with positive finite
// durations and distinct priorities per source, conditions that name a parameter of the matching type, and finite numbers. Clips and joints
// are checked when a machine is created, because the graph does not know them.
[[nodiscard]] std::optional<std::string> validate_state_graph(const StateGraph& graph);

// Parses the JSON form. A wrong format or version, a missing key, a wrong type, or a graph that fails validate_state_graph is an error.
[[nodiscard]] core::Result<StateGraph, std::string> parse_state_graph(std::string_view text);

// The JSON form of a graph. parse_state_graph(state_graph_json(g)) gives back g.
[[nodiscard]] std::string state_graph_json(const StateGraph& graph);

}  // namespace engine::animation
