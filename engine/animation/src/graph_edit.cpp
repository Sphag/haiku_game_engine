#include "engine/animation/graph_edit.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace engine::animation {
namespace {

constexpr float kDefaultTransitionDuration = 0.25f;

GraphEditResult refuse(std::string message) {
    return core::failure(std::move(message));
}

std::optional<std::size_t> find_state(const StateGraph& graph, std::string_view name) {
    for (std::size_t i = 0; i < graph.states.size(); ++i) {
        if (graph.states[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

bool has_parameter(const StateGraph& graph, std::string_view name) {
    return std::any_of(graph.parameters.begin(), graph.parameters.end(), [&](const ParamDecl& p) { return p.name == name; });
}

// The index of the first transition whose condition tests `name`, if any.
std::optional<std::size_t> transition_using(const StateGraph& graph, std::string_view name) {
    for (std::size_t t = 0; t < graph.transitions.size(); ++t) {
        for (const Condition& condition : graph.transitions[t].conditions) {
            if (condition.kind != ConditionKind::Finished && condition.parameter == name) {
                return t;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

AnimationGraphDocument::AnimationGraphDocument(State state, ClipLibrary clips) : state_(std::move(state)), clips_(std::move(clips)) {}

core::Result<AnimationGraphDocument, std::string> AnimationGraphDocument::create(Skeleton skeleton, ClipLibrary clips) {
    std::optional<std::string> first;
    for (const auto& [name, clip] : clips) {
        if (!validate_clip(clip, skeleton)) {
            first = name;
            break;
        }
    }
    if (!first) {
        return core::failure(std::string("no clip in the library is valid for the skeleton, so a new graph has no default state"));
    }

    State state;
    state.graph.states.push_back(StateDecl{"default", *first, 1.0f});
    state.graph.default_state = "default";
    state.skeleton = std::move(skeleton);

    AnimationGraphDocument document(std::move(state), std::move(clips));
    if (auto checked = StateMachine::create(document.state_.graph, document.state_.skeleton, document.clips_); !checked.ok()) {
        return core::failure(checked.error());
    }
    return core::success(std::move(document));
}

core::Result<AnimationGraphDocument, std::string> AnimationGraphDocument::open(std::string_view graph_json, Skeleton skeleton,
                                                                              ClipLibrary clips) {
    auto parsed = parse_state_graph(graph_json);
    if (!parsed.ok()) {
        return core::failure(parsed.error());
    }

    State state;
    state.graph = std::move(parsed).value();
    state.skeleton = std::move(skeleton);
    AnimationGraphDocument document(std::move(state), std::move(clips));
    if (auto checked = StateMachine::create(document.state_.graph, document.state_.skeleton, document.clips_); !checked.ok()) {
        return core::failure(checked.error());
    }
    return core::success(std::move(document));
}

std::vector<std::string> AnimationGraphDocument::compatible_clips() const {
    std::vector<std::string> names;
    for (const auto& [name, clip] : clips_) {
        if (!validate_clip(clip, state_.skeleton)) {
            names.push_back(name);
        }
    }
    return names;
}

GraphEditResult AnimationGraphDocument::commit(State next) {
    if (auto checked = StateMachine::create(next.graph, next.skeleton, clips_); !checked.ok()) {
        return refuse(checked.error());
    }
    undo_.push_back(std::move(state_));
    state_ = std::move(next);
    redo_.clear();
    ++revision_;
    return core::success();
}

GraphEditResult AnimationGraphDocument::add_state(const std::string& name, const std::string& clip) {
    if (name.empty()) {
        return refuse("a state needs a name");
    }
    if (find_state(state_.graph, name)) {
        return refuse("a state named \"" + name + "\" already exists");
    }
    State next = state_;
    next.graph.states.push_back(StateDecl{name, clip, 1.0f});
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::remove_state(const std::string& name) {
    const auto index = find_state(state_.graph, name);
    if (!index) {
        return refuse("no state named \"" + name + "\"");
    }
    if (state_.graph.default_state == name) {
        return refuse("\"" + name + "\" is the default state; choose another default before removing it");
    }
    State next = state_;
    next.graph.states.erase(next.graph.states.begin() + static_cast<std::ptrdiff_t>(*index));
    auto& transitions = next.graph.transitions;
    transitions.erase(std::remove_if(transitions.begin(), transitions.end(),
                                     [&](const TransitionDecl& t) { return t.from == name || t.to == name; }),
                      transitions.end());
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::rename_state(const std::string& from, const std::string& to) {
    const auto index = find_state(state_.graph, from);
    if (!index) {
        return refuse("no state named \"" + from + "\"");
    }
    if (to.empty()) {
        return refuse("a state needs a name");
    }
    if (from != to && find_state(state_.graph, to)) {
        return refuse("a state named \"" + to + "\" already exists");
    }
    State next = state_;
    next.graph.states[*index].name = to;
    for (TransitionDecl& t : next.graph.transitions) {
        if (t.from == from) {
            t.from = to;
        }
        if (t.to == from) {
            t.to = to;
        }
    }
    if (next.graph.default_state == from) {
        next.graph.default_state = to;
    }
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::set_state_clip(const std::string& name, const std::string& clip) {
    const auto index = find_state(state_.graph, name);
    if (!index) {
        return refuse("no state named \"" + name + "\"");
    }
    State next = state_;
    next.graph.states[*index].clip = clip;
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::set_state_speed(const std::string& name, float speed) {
    const auto index = find_state(state_.graph, name);
    if (!index) {
        return refuse("no state named \"" + name + "\"");
    }
    State next = state_;
    next.graph.states[*index].speed = speed;
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::set_default_state(const std::string& name) {
    if (!find_state(state_.graph, name)) {
        return refuse("no state named \"" + name + "\"");
    }
    State next = state_;
    next.graph.default_state = name;
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::add_parameter(const ParamDecl& parameter) {
    if (parameter.name.empty()) {
        return refuse("a parameter needs a name");
    }
    if (has_parameter(state_.graph, parameter.name)) {
        return refuse("a parameter named \"" + parameter.name + "\" already exists");
    }
    State next = state_;
    next.graph.parameters.push_back(parameter);
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::remove_parameter(const std::string& name) {
    if (!has_parameter(state_.graph, name)) {
        return refuse("no parameter named \"" + name + "\"");
    }
    if (const auto user = transition_using(state_.graph, name)) {
        return refuse("parameter \"" + name + "\" is tested by transition " + std::to_string(*user) + "; remove that condition first");
    }
    State next = state_;
    auto& parameters = next.graph.parameters;
    parameters.erase(std::remove_if(parameters.begin(), parameters.end(), [&](const ParamDecl& p) { return p.name == name; }),
                     parameters.end());
    return commit(std::move(next));
}

core::Result<std::size_t, std::string> AnimationGraphDocument::add_transition(const std::string& from, const std::string& to) {
    if (!find_state(state_.graph, from)) {
        return core::failure("no state named \"" + from + "\"");
    }
    if (!find_state(state_.graph, to)) {
        return core::failure("no state named \"" + to + "\"");
    }
    int priority = 0;
    for (const TransitionDecl& t : state_.graph.transitions) {
        if (t.from == from) {
            priority = std::max(priority, t.priority + 1);
        }
    }
    State next = state_;
    next.graph.transitions.push_back(TransitionDecl{from, to, kDefaultTransitionDuration, priority, {}});
    const std::size_t index = next.graph.transitions.size() - 1;
    if (auto applied = commit(std::move(next)); !applied.ok()) {
        return core::failure(applied.error());
    }
    return core::success(index);
}

GraphEditResult AnimationGraphDocument::remove_transition(std::size_t index) {
    if (index >= state_.graph.transitions.size()) {
        return refuse("no transition " + std::to_string(index));
    }
    State next = state_;
    next.graph.transitions.erase(next.graph.transitions.begin() + static_cast<std::ptrdiff_t>(index));
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::set_transition_timing(std::size_t index, float duration, int priority) {
    if (index >= state_.graph.transitions.size()) {
        return refuse("no transition " + std::to_string(index));
    }
    State next = state_;
    next.graph.transitions[index].duration = duration;
    next.graph.transitions[index].priority = priority;
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::add_condition(std::size_t transition, const Condition& condition) {
    if (transition >= state_.graph.transitions.size()) {
        return refuse("no transition " + std::to_string(transition));
    }
    State next = state_;
    next.graph.transitions[transition].conditions.push_back(condition);
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::set_condition(std::size_t transition, std::size_t condition_index, const Condition& condition) {
    if (transition >= state_.graph.transitions.size()) {
        return refuse("no transition " + std::to_string(transition));
    }
    auto& conditions = state_.graph.transitions[transition].conditions;
    if (condition_index >= conditions.size()) {
        return refuse("transition " + std::to_string(transition) + " has no condition " + std::to_string(condition_index));
    }
    State next = state_;
    next.graph.transitions[transition].conditions[condition_index] = condition;
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::remove_condition(std::size_t transition, std::size_t condition_index) {
    if (transition >= state_.graph.transitions.size()) {
        return refuse("no transition " + std::to_string(transition));
    }
    if (condition_index >= state_.graph.transitions[transition].conditions.size()) {
        return refuse("transition " + std::to_string(transition) + " has no condition " + std::to_string(condition_index));
    }
    State next = state_;
    auto& conditions = next.graph.transitions[transition].conditions;
    conditions.erase(conditions.begin() + static_cast<std::ptrdiff_t>(condition_index));
    return commit(std::move(next));
}

GraphEditResult AnimationGraphDocument::set_skeleton(Skeleton skeleton) {
    State next = state_;
    next.skeleton = std::move(skeleton);
    return commit(std::move(next));
}

bool AnimationGraphDocument::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(std::move(state_));
    state_ = std::move(undo_.back());
    undo_.pop_back();
    ++revision_;
    return true;
}

bool AnimationGraphDocument::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(std::move(state_));
    state_ = std::move(redo_.back());
    redo_.pop_back();
    ++revision_;
    return true;
}

std::string AnimationGraphDocument::save() const {
    return state_graph_json(state_.graph);
}

core::Result<StateMachine, std::string> AnimationGraphDocument::preview() const {
    return StateMachine::create(state_.graph, state_.skeleton, clips_);
}

}  // namespace engine::animation
