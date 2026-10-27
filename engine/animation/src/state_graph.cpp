#include "engine/animation/state_graph.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace engine::animation {
namespace {

using Json = nlohmann::json;

// A problem in the file's content. Only parse_state_graph throws it, and it turns the text into the error result.
struct GraphError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr const char* kOpNames[] = {"<", "<=", ">", ">=", "==", "!="};

const Json& required(const Json& object, const char* key, const std::string& where) {
    if (!object.is_object() || !object.contains(key)) {
        throw GraphError(where + " is missing \"" + key + "\"");
    }
    return object.at(key);
}

std::string text_of(const Json& object, const char* key, const std::string& where) {
    const Json& value = required(object, key, where);
    if (!value.is_string()) {
        throw GraphError(where + " has a \"" + key + "\" that is not a string");
    }
    return value.get<std::string>();
}

bool bool_of(const Json& object, const char* key, const std::string& where) {
    const Json& value = required(object, key, where);
    if (!value.is_boolean()) {
        throw GraphError(where + " has a \"" + key + "\" that is not a boolean");
    }
    return value.get<bool>();
}

float float_of(const Json& object, const char* key, const std::string& where) {
    const Json& value = required(object, key, where);
    if (!value.is_number()) {
        throw GraphError(where + " has a \"" + key + "\" that is not a number");
    }
    return value.get<float>();
}

int int_of(const Json& object, const char* key, const std::string& where) {
    const Json& value = required(object, key, where);
    if (!value.is_number_integer()) {
        throw GraphError(where + " has a \"" + key + "\" that is not an integer");
    }
    const std::int64_t number = value.get<std::int64_t>();
    if (number < INT32_MIN || number > INT32_MAX) {
        throw GraphError(where + " has a \"" + key + "\" outside the 32-bit range");
    }
    return static_cast<int>(number);
}

const Json& array_of(const Json& object, const char* key, const std::string& where) {
    const Json& value = required(object, key, where);
    if (!value.is_array()) {
        throw GraphError(where + " has a \"" + key + "\" that is not an array");
    }
    return value;
}

CompareOp op_from_text(const std::string& text, const std::string& where) {
    for (std::size_t i = 0; i < sizeof(kOpNames) / sizeof(kOpNames[0]); ++i) {
        if (text == kOpNames[i]) {
            return static_cast<CompareOp>(i);
        }
    }
    throw GraphError(where + " has an unknown comparison \"" + text + "\"");
}

ParamType type_from_text(const std::string& text, const std::string& where) {
    if (text == "float") {
        return ParamType::Float;
    }
    if (text == "bool") {
        return ParamType::Bool;
    }
    if (text == "trigger") {
        return ParamType::Trigger;
    }
    throw GraphError(where + " has an unknown parameter type \"" + text + "\"");
}

const char* type_text(ParamType type) {
    switch (type) {
        case ParamType::Float:
            return "float";
        case ParamType::Bool:
            return "bool";
        case ParamType::Trigger:
            return "trigger";
    }
    return "float";
}

Condition condition_from_json(const Json& json, const std::string& where) {
    Condition condition;
    const std::string when = text_of(json, "when", where);
    if (when == "finished") {
        condition.kind = ConditionKind::Finished;
    } else if (when == "float") {
        condition.kind = ConditionKind::Float;
        condition.parameter = text_of(json, "parameter", where);
        condition.op = op_from_text(text_of(json, "op", where), where);
        condition.value = float_of(json, "value", where);
    } else if (when == "bool") {
        condition.kind = ConditionKind::Bool;
        condition.parameter = text_of(json, "parameter", where);
        condition.expected = bool_of(json, "is", where);
    } else if (when == "trigger") {
        condition.kind = ConditionKind::Trigger;
        condition.parameter = text_of(json, "parameter", where);
    } else {
        throw GraphError(where + " has an unknown condition \"" + when + "\"");
    }
    return condition;
}

Json condition_to_json(const Condition& condition) {
    switch (condition.kind) {
        case ConditionKind::Finished:
            return Json{{"when", "finished"}};
        case ConditionKind::Float:
            return Json{{"when", "float"}, {"parameter", condition.parameter}, {"op", kOpNames[static_cast<int>(condition.op)]},
                        {"value", condition.value}};
        case ConditionKind::Bool:
            return Json{{"when", "bool"}, {"parameter", condition.parameter}, {"is", condition.expected}};
        case ConditionKind::Trigger:
            return Json{{"when", "trigger"}, {"parameter", condition.parameter}};
    }
    return Json::object();
}

StateGraph graph_from_json(const Json& doc) {
    const std::string where = "the graph";
    if (!doc.is_object()) {
        throw GraphError("the graph is not a JSON object");
    }
    const std::string format = text_of(doc, "format", where);
    if (format != kGraphFormat) {
        throw GraphError("the format is \"" + format + "\", not \"engine.animation_graph\"");
    }
    const int version = int_of(doc, "version", where);
    if (version != static_cast<int>(kGraphVersion)) {
        throw GraphError("version " + std::to_string(version) + " is not supported; this build reads version 1");
    }

    StateGraph graph;
    for (const Json& item : array_of(doc, "parameters", where)) {
        const std::string at = "a parameter";
        ParamDecl param;
        param.name = text_of(item, "name", at);
        param.type = type_from_text(text_of(item, "type", at), at);
        if (item.contains("default")) {
            if (param.type == ParamType::Bool) {
                param.default_bool = bool_of(item, "default", at);
            } else if (param.type == ParamType::Float) {
                param.default_float = float_of(item, "default", at);
            } else {
                throw GraphError("trigger parameter \"" + param.name + "\" cannot have a default");
            }
        }
        graph.parameters.push_back(std::move(param));
    }

    graph.default_state = text_of(doc, "default_state", where);

    if (doc.contains("root")) {
        const Json& root = doc.at("root");
        const std::string at = "the root";
        graph.root.joint = text_of(root, "joint", at);
        graph.root.translate_x = bool_of(root, "translate_x", at);
        graph.root.translate_y = bool_of(root, "translate_y", at);
        graph.root.translate_z = bool_of(root, "translate_z", at);
        graph.root.yaw = bool_of(root, "yaw", at);
    }

    for (const Json& item : array_of(doc, "states", where)) {
        const std::string at = "a state";
        StateDecl state;
        state.name = text_of(item, "name", at);
        state.clip = text_of(item, "clip", at);
        state.speed = float_of(item, "speed", at);
        graph.states.push_back(std::move(state));
    }

    for (const Json& item : array_of(doc, "transitions", where)) {
        const std::string at = "a transition";
        TransitionDecl transition;
        transition.from = text_of(item, "from", at);
        transition.to = text_of(item, "to", at);
        transition.duration = float_of(item, "duration", at);
        transition.priority = int_of(item, "priority", at);
        for (const Json& condition : array_of(item, "conditions", at)) {
            transition.conditions.push_back(condition_from_json(condition, "a condition"));
        }
        graph.transitions.push_back(std::move(transition));
    }
    return graph;
}

}  // namespace

std::optional<std::string> validate_state_graph(const StateGraph& graph) {
    if (graph.states.empty()) {
        return std::string("the graph has no states");
    }

    std::map<std::string, ParamType> params;
    for (const ParamDecl& param : graph.parameters) {
        if (param.name.empty()) {
            return std::string("a parameter has an empty name");
        }
        if (params.count(param.name) != 0) {
            return "parameter \"" + param.name + "\" is declared twice";
        }
        if (param.type == ParamType::Float && !std::isfinite(param.default_float)) {
            return "parameter \"" + param.name + "\" has a non-finite default";
        }
        params.emplace(param.name, param.type);
    }

    std::set<std::string> states;
    for (const StateDecl& state : graph.states) {
        if (state.name.empty()) {
            return std::string("a state has an empty name");
        }
        if (!states.insert(state.name).second) {
            return "state \"" + state.name + "\" is declared twice";
        }
        if (state.clip.empty()) {
            return "state \"" + state.name + "\" names no clip";
        }
        if (!std::isfinite(state.speed)) {
            return "state \"" + state.name + "\" has a non-finite speed";
        }
    }
    if (states.count(graph.default_state) == 0) {
        return "the default state \"" + graph.default_state + "\" is not a state of the graph";
    }

    std::set<std::pair<std::string, int>> priorities;
    for (const TransitionDecl& transition : graph.transitions) {
        const std::string label = "the transition " + transition.from + " -> " + transition.to;
        if (states.count(transition.from) == 0) {
            return label + " leaves a state that does not exist";
        }
        if (states.count(transition.to) == 0) {
            return label + " enters a state that does not exist";
        }
        if (!std::isfinite(transition.duration) || !(transition.duration > 0.0f)) {
            return label + " needs a duration greater than zero";
        }
        if (!priorities.insert({transition.from, transition.priority}).second) {
            return label + " shares priority " + std::to_string(transition.priority) + " with another transition from " + transition.from;
        }
        for (const Condition& condition : transition.conditions) {
            if (condition.kind == ConditionKind::Finished) {
                continue;
            }
            const auto found = params.find(condition.parameter);
            if (found == params.end()) {
                return label + " tests the unknown parameter \"" + condition.parameter + "\"";
            }
            const ParamType expected = condition.kind == ConditionKind::Float   ? ParamType::Float
                                       : condition.kind == ConditionKind::Bool ? ParamType::Bool
                                                                               : ParamType::Trigger;
            if (found->second != expected) {
                return label + " tests \"" + condition.parameter + "\" as " + type_text(expected) + ", but it is " +
                       type_text(found->second);
            }
            if (condition.kind == ConditionKind::Float && !std::isfinite(condition.value)) {
                return label + " compares \"" + condition.parameter + "\" with a non-finite value";
            }
        }
    }
    return std::nullopt;
}

core::Result<StateGraph, std::string> parse_state_graph(std::string_view text) {
    StateGraph graph;
    try {
        graph = graph_from_json(Json::parse(std::string(text)));
    } catch (const Json::exception& error) {
        return core::failure(std::string("the graph is not valid JSON: ") + error.what());
    } catch (const GraphError& error) {
        return core::failure(std::string(error.what()));
    }
    if (auto problem = validate_state_graph(graph)) {
        return core::failure(*problem);
    }
    return core::success(std::move(graph));
}

std::string state_graph_json(const StateGraph& graph) {
    Json doc;
    doc["format"] = std::string(kGraphFormat);
    doc["version"] = kGraphVersion;
    doc["default_state"] = graph.default_state;

    Json parameters = Json::array();
    for (const ParamDecl& param : graph.parameters) {
        Json item{{"name", param.name}, {"type", type_text(param.type)}};
        if (param.type == ParamType::Bool) {
            item["default"] = param.default_bool;
        } else if (param.type == ParamType::Float) {
            item["default"] = param.default_float;
        }
        parameters.push_back(std::move(item));
    }
    doc["parameters"] = std::move(parameters);

    if (!graph.root.joint.empty()) {
        doc["root"] = Json{{"joint", graph.root.joint}, {"translate_x", graph.root.translate_x}, {"translate_y", graph.root.translate_y},
                           {"translate_z", graph.root.translate_z}, {"yaw", graph.root.yaw}};
    }

    Json states = Json::array();
    for (const StateDecl& state : graph.states) {
        states.push_back(Json{{"name", state.name}, {"clip", state.clip}, {"speed", state.speed}});
    }
    doc["states"] = std::move(states);

    Json transitions = Json::array();
    for (const TransitionDecl& transition : graph.transitions) {
        Json conditions = Json::array();
        for (const Condition& condition : transition.conditions) {
            conditions.push_back(condition_to_json(condition));
        }
        transitions.push_back(Json{{"from", transition.from}, {"to", transition.to}, {"duration", transition.duration},
                                   {"priority", transition.priority}, {"conditions", std::move(conditions)}});
    }
    doc["transitions"] = std::move(transitions);
    return doc.dump();
}

}  // namespace engine::animation
