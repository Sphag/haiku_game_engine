#include "engine/animation/state_machine.hpp"

#include "engine/core/assert.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace engine::animation {
namespace {

std::optional<std::size_t> state_index(const StateGraph& graph, const std::string& name) {
    for (std::size_t i = 0; i < graph.states.size(); ++i) {
        if (graph.states[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> parameter_index(const StateGraph& graph, const std::string& name) {
    for (std::size_t i = 0; i < graph.parameters.size(); ++i) {
        if (graph.parameters[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace

core::Result<StateMachine, std::string> StateMachine::create(const StateGraph& graph, const Skeleton& skeleton, const ClipLibrary& clips) {
    if (auto problem = validate_state_graph(graph)) {
        return core::failure(*problem);
    }
    if (auto problem = validate_skeleton(skeleton)) {
        return core::failure("the skeleton is not valid: " + *problem);
    }

    StateMachine machine;
    machine.graph_ = graph;
    machine.skeleton_ = skeleton;

    // Without a root joint the root flags are all off, so root_displacement reads joint 0 and reports no motion.
    machine.root_motion_ = RootMotion{0, false, false, false, false};
    if (!graph.root.joint.empty()) {
        const auto found = std::find(skeleton.names.begin(), skeleton.names.end(), graph.root.joint);
        if (found == skeleton.names.end()) {
            return core::failure("the root joint \"" + graph.root.joint + "\" is not a joint of the skeleton");
        }
        machine.root_motion_ = RootMotion{static_cast<std::uint32_t>(found - skeleton.names.begin()), graph.root.translate_x,
                                          graph.root.translate_y, graph.root.translate_z, graph.root.yaw};
    }

    machine.clips_.reserve(graph.states.size());
    for (const StateDecl& state : graph.states) {
        const auto clip = clips.find(state.clip);
        if (clip == clips.end()) {
            return core::failure("state \"" + state.name + "\" names clip \"" + state.clip + "\", which is not in the library");
        }
        if (auto problem = validate_clip(clip->second, skeleton)) {
            return core::failure("clip \"" + state.clip + "\": " + *problem);
        }
        machine.clips_.push_back(clip->second);
    }

    machine.params_.reserve(graph.parameters.size());
    for (const ParamDecl& param : graph.parameters) {
        Param value;
        value.type = param.type;
        value.number = param.default_float;
        value.flag = param.default_bool;
        machine.params_.push_back(value);
    }

    machine.outgoing_.assign(graph.states.size(), {});
    machine.conditions_.reserve(graph.transitions.size());
    machine.targets_.reserve(graph.transitions.size());
    for (std::size_t t = 0; t < graph.transitions.size(); ++t) {
        const TransitionDecl& transition = graph.transitions[t];
        std::vector<ResolvedCondition> resolved;
        for (const Condition& condition : transition.conditions) {
            ResolvedCondition out;
            out.kind = condition.kind;
            if (condition.kind != ConditionKind::Finished) {
                out.param = *parameter_index(graph, condition.parameter);  // checked by validate_state_graph
            }
            out.op = condition.op;
            out.value = condition.value;
            out.expected = condition.expected;
            resolved.push_back(out);
        }
        machine.conditions_.push_back(std::move(resolved));
        const std::size_t from = *state_index(graph, transition.from);  // checked by validate_state_graph
        machine.targets_.push_back(static_cast<std::uint32_t>(*state_index(graph, transition.to)));
        machine.outgoing_[from].push_back(t);
    }
    // Distinct priorities per source (validated), so the order is total and no file order is involved.
    for (std::vector<std::size_t>& list : machine.outgoing_) {
        std::sort(list.begin(), list.end(), [&graph](std::size_t a, std::size_t b) {
            return graph.transitions[a].priority > graph.transitions[b].priority;
        });
    }

    machine.active_ = static_cast<std::uint32_t>(*state_index(graph, graph.default_state));
    machine.events_.push_back(AnimationEvent{AnimationEvent::Kind::Entered, machine.active_, 0});
    machine.evaluate();
    return core::success(std::move(machine));
}

void StateMachine::update(float dt) {
    ENGINE_ASSERT(std::isfinite(dt) && dt >= 0.0f, "the state machine update needs a finite, non-negative time step");
    advance(dt);
    if (!blending_) {
        for (const std::size_t transition : outgoing_[active_]) {
            const std::vector<ResolvedCondition>& conditions = conditions_[transition];
            const bool holds = std::all_of(conditions.begin(), conditions.end(), [this](const ResolvedCondition& c) { return condition_holds(c); });
            if (holds) {
                take_transition(transition);
                break;  // the highest-priority satisfied transition wins; the rest wait for the next update
            }
        }
    }
    evaluate();
}

void StateMachine::advance(float dt) {
    if (blending_) {
        source_elapsed_ += dt * graph_.states[source_].speed;
        blend_time_ += dt;
    }
    elapsed_ += dt * graph_.states[active_].speed;
    if (blending_ && blend_time_ >= blend_duration_) {
        blending_ = false;
    }

    // One loop event for each cycle boundary the clip crossed, in the direction it moved.
    const std::int64_t cycle = cycle_at(elapsed_);
    const std::int64_t step = cycle > cycle_ ? 1 : -1;
    for (std::int64_t c = cycle_; c != cycle; c += step) {
        events_.push_back(AnimationEvent{AnimationEvent::Kind::Loop, active_, c + step});
    }
    cycle_ = cycle;

    const bool done = finished_now();
    if (done && !finished_) {
        events_.push_back(AnimationEvent{AnimationEvent::Kind::Finished, active_, 0});
    }
    finished_ = done;
}

void StateMachine::take_transition(std::size_t transition) {
    // A trigger is consumed by the transition that tests it. Triggers a transition does not test stay set.
    for (const ResolvedCondition& condition : conditions_[transition]) {
        if (condition.kind == ConditionKind::Trigger) {
            params_[condition.param].flag = false;
        }
    }
    source_ = active_;
    source_elapsed_ = elapsed_;
    active_ = targets_[transition];
    elapsed_ = 0.0f;
    cycle_ = 0;
    finished_ = false;
    blending_ = true;
    blend_time_ = 0.0f;
    blend_duration_ = graph_.transitions[transition].duration;
    events_.push_back(AnimationEvent{AnimationEvent::Kind::Entered, active_, 0});
}

void StateMachine::evaluate() {
    const AnimationClip& entering = clips_[active_];
    BlendSample sample = blending_ ? sample_blend(skeleton_, clips_[source_], source_elapsed_, entering, elapsed_, blend_weight(), root_motion_)
                                   : sample_blend(skeleton_, entering, elapsed_, entering, elapsed_, 0.0f, root_motion_);
    // The delta is taken against the previous total, so a state change that keeps the pose keeps the root continuous.
    root_delta_.translation = sample.root.translation - root_total_.translation;
    root_delta_.yaw = sample.root.yaw - root_total_.yaw;
    root_total_ = sample.root;
    pose_ = std::move(sample.pose);
}

bool StateMachine::condition_holds(const ResolvedCondition& condition) const {
    const Param& param = params_[condition.param];
    switch (condition.kind) {
        case ConditionKind::Finished:
            return finished_;
        case ConditionKind::Trigger:
            return param.flag;
        case ConditionKind::Bool:
            return param.flag == condition.expected;
        case ConditionKind::Float:
            switch (condition.op) {
                case CompareOp::Less:
                    return param.number < condition.value;
                case CompareOp::LessEqual:
                    return param.number <= condition.value;
                case CompareOp::Greater:
                    return param.number > condition.value;
                case CompareOp::GreaterEqual:
                    return param.number >= condition.value;
                case CompareOp::Equal:
                    return param.number == condition.value;
                case CompareOp::NotEqual:
                    return param.number != condition.value;
            }
            return false;
    }
    return false;
}

bool StateMachine::finished_now() const noexcept {
    const AnimationClip& clip = clips_[active_];
    if (clip.looping) {
        return false;
    }
    const float speed = graph_.states[active_].speed;
    return speed >= 0.0f ? elapsed_ >= clip.duration : elapsed_ <= 0.0f;
}

std::int64_t StateMachine::cycle_at(float elapsed) const noexcept {
    const AnimationClip& clip = clips_[active_];
    if (!clip.looping) {
        return 0;
    }
    return static_cast<std::int64_t>(std::floor(static_cast<double>(elapsed) / static_cast<double>(clip.duration)));
}

std::optional<std::size_t> StateMachine::param_index(std::string_view name) const {
    for (std::size_t i = 0; i < graph_.parameters.size(); ++i) {
        if (graph_.parameters[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

float StateMachine::blend_weight() const noexcept {
    if (!blending_) {
        return 1.0f;
    }
    return std::clamp(blend_time_ / blend_duration_, 0.0f, 1.0f);
}

std::string_view StateMachine::state_name() const noexcept {
    return graph_.states[active_].name;
}

bool StateMachine::set_float(std::string_view name, float value) {
    const auto index = param_index(name);
    if (!index || params_[*index].type != ParamType::Float) {
        return false;
    }
    params_[*index].number = value;
    return true;
}

bool StateMachine::set_bool(std::string_view name, bool value) {
    const auto index = param_index(name);
    if (!index || params_[*index].type != ParamType::Bool) {
        return false;
    }
    params_[*index].flag = value;
    return true;
}

bool StateMachine::set_trigger(std::string_view name) {
    const auto index = param_index(name);
    if (!index || params_[*index].type != ParamType::Trigger) {
        return false;
    }
    params_[*index].flag = true;
    return true;
}

std::optional<float> StateMachine::get_float(std::string_view name) const {
    const auto index = param_index(name);
    if (!index || params_[*index].type != ParamType::Float) {
        return std::nullopt;
    }
    return params_[*index].number;
}

std::optional<bool> StateMachine::get_bool(std::string_view name) const {
    const auto index = param_index(name);
    if (!index || params_[*index].type != ParamType::Bool) {
        return std::nullopt;
    }
    return params_[*index].flag;
}

std::optional<bool> StateMachine::get_trigger(std::string_view name) const {
    const auto index = param_index(name);
    if (!index || params_[*index].type != ParamType::Trigger) {
        return std::nullopt;
    }
    return params_[*index].flag;
}

std::vector<AnimationEvent> StateMachine::take_events() {
    std::vector<AnimationEvent> out;
    out.swap(events_);
    return out;
}

bool AnimatorSet::add(std::string name, StateMachine machine) {
    if (machines_.find(name) != machines_.end()) {
        return false;
    }
    machines_.emplace(std::move(name), std::make_unique<StateMachine>(std::move(machine)));
    return true;
}

StateMachine* AnimatorSet::find(std::string_view name) noexcept {
    const auto found = machines_.find(name);
    return found == machines_.end() ? nullptr : found->second.get();
}

bool AnimatorSet::remove(std::string_view name) {
    const auto found = machines_.find(name);
    if (found == machines_.end()) {
        return false;
    }
    machines_.erase(found);
    return true;
}

void AnimatorSet::update(float dt) {
    for (auto& entry : machines_) {
        entry.second->update(dt);
    }
}

}  // namespace engine::animation
