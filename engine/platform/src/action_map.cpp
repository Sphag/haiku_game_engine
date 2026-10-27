#include "engine/platform/action_map.hpp"

#include "engine/core/assert.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace engine::platform {
namespace {

ActionError make_error(ActionErrorCode code, std::string message) {
    return ActionError{code, std::move(message)};
}

// The first reason a binding cannot be used, or nullopt when it is valid.
std::optional<std::string> binding_problem(const Binding& binding) {
    if (!std::isfinite(binding.scale) || binding.scale == 0.0f) {
        return "scale must be a finite, nonzero number";
    }
    if (!(binding.dead_zone >= 0.0f && binding.dead_zone < 1.0f)) {
        return "dead zone must be at least 0 and below 1";
    }

    if (const auto* key = std::get_if<KeySource>(&binding.source)) {
        if (key->key == Key::Unknown || static_cast<std::size_t>(key->key) >= kKeyCount) {
            return "unknown key";
        }
    } else if (const auto* button = std::get_if<MouseButtonSource>(&binding.source)) {
        if (static_cast<std::size_t>(button->button) >= kMouseButtonCount) {
            return "unknown mouse button";
        }
    } else if (const auto* motion = std::get_if<MouseAxisSource>(&binding.source)) {
        if (static_cast<std::size_t>(motion->axis) >= kMouseAxisCount) {
            return "unknown mouse axis";
        }
    } else if (const auto* pad_button = std::get_if<GamepadButtonSource>(&binding.source)) {
        if (static_cast<std::size_t>(pad_button->button) >= kGamepadButtonCount) {
            return "unknown gamepad button";
        }
        if (pad_button->slot && *pad_button->slot >= kMaxGamepads) {
            return "gamepad slot must be below " + std::to_string(kMaxGamepads);
        }
    } else if (const auto* pad_axis = std::get_if<GamepadAxisSource>(&binding.source)) {
        if (static_cast<std::size_t>(pad_axis->axis) >= kGamepadAxisCount) {
            return "unknown gamepad axis";
        }
        if (pad_axis->slot && *pad_axis->slot >= kMaxGamepads) {
            return "gamepad slot must be below " + std::to_string(kMaxGamepads);
        }
    }

    if (binding.dead_zone != 0.0f && !std::holds_alternative<GamepadAxisSource>(binding.source)) {
        return "dead zone applies to gamepad axes only";
    }
    return std::nullopt;
}

// The first invalid binding of an action, described for the caller. Empty when all bindings are valid.
std::optional<std::string> bindings_problem(std::string_view action, const std::vector<Binding>& bindings) {
    for (const Binding& binding : bindings) {
        if (auto problem = binding_problem(binding)) {
            return "action '" + std::string(action) + "': " + *problem;
        }
    }
    return std::nullopt;
}

// The range of gamepad slots a source reads: its own slot, or every slot when it reads any pad.
std::pair<std::uint32_t, std::uint32_t> slot_range(const std::optional<std::uint32_t>& slot) {
    if (slot) {
        return {*slot, *slot + 1};
    }
    return {0, kMaxGamepads};
}

// What one binding contributes this frame, before dead zone and scale. A pad read through any slot takes the
// strongest value, so a pad in any slot can drive the action.
float raw_source_value(const SourceData& source, const InputState& input) {
    if (const auto* key = std::get_if<KeySource>(&source)) {
        return input.key_down(key->key) ? 1.0f : 0.0f;
    }
    if (const auto* button = std::get_if<MouseButtonSource>(&source)) {
        return input.mouse_down(button->button) ? 1.0f : 0.0f;
    }
    if (const auto* motion = std::get_if<MouseAxisSource>(&source)) {
        const InputState::MouseDelta delta = input.mouse_delta();
        switch (motion->axis) {
            case MouseAxis::MotionX:
                return static_cast<float>(delta.dx);
            case MouseAxis::MotionY:
                return static_cast<float>(delta.dy);
            case MouseAxis::WheelX:
                return static_cast<float>(input.wheel_x());
            case MouseAxis::WheelY:
                return static_cast<float>(input.wheel_y());
            case MouseAxis::Count:
                break;
        }
        return 0.0f;
    }
    if (const auto* pad_button = std::get_if<GamepadButtonSource>(&source)) {
        const auto [first, end] = slot_range(pad_button->slot);
        for (std::uint32_t slot = first; slot < end; ++slot) {
            if (input.gamepad_down(slot, pad_button->button)) {
                return 1.0f;
            }
        }
        return 0.0f;
    }
    const auto& pad_axis = std::get<GamepadAxisSource>(source);
    const auto [first, end] = slot_range(pad_axis.slot);
    float strongest = 0.0f;
    for (std::uint32_t slot = first; slot < end; ++slot) {
        const float value = input.gamepad_axis(slot, pad_axis.axis);
        if (std::fabs(value) > std::fabs(strongest)) {
            strongest = value;
        }
    }
    return strongest;
}

// What one binding adds to its action this frame.
float contribution(const Binding& binding, const InputState& input) {
    float value = raw_source_value(binding.source, input);
    if (std::holds_alternative<GamepadAxisSource>(binding.source)) {
        value = apply_axis_dead_zone(value, binding.dead_zone);
    }
    return value * binding.scale;
}

}  // namespace

float apply_axis_dead_zone(float value, float dead_zone) noexcept {
    if (dead_zone <= 0.0f) {
        return value;
    }
    const float magnitude = std::fabs(value);
    if (magnitude <= dead_zone) {
        return 0.0f;
    }
    // Rescale so the range just outside the dead zone starts at 0 and full deflection still reads 1.
    const float rescaled = std::min((magnitude - dead_zone) / (1.0f - dead_zone), 1.0f);
    return std::copysign(rescaled, value);
}

core::Result<ActionId, ActionError> ActionMap::add_action(std::string name, ActionKind kind,
                                                          std::vector<Binding> default_bindings) {
    if (name.empty()) {
        return core::failure(make_error(ActionErrorCode::EmptyName, "action name is empty"));
    }
    if (index_.contains(name)) {
        return core::failure(make_error(ActionErrorCode::DuplicateName, "action '" + name + "' is already defined"));
    }
    if (auto problem = bindings_problem(name, default_bindings)) {
        return core::failure(make_error(ActionErrorCode::InvalidBinding, std::move(*problem)));
    }

    const auto id = static_cast<ActionId>(entries_.size());
    index_.emplace(name, id);
    entries_.push_back(Entry{ActionDefinition{std::move(name), kind, std::move(default_bindings)}, std::nullopt});
    return core::success(id);
}

std::optional<ActionId> ActionMap::find(std::string_view name) const {
    const auto found = index_.find(name);
    if (found == index_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::size_t ActionMap::size() const noexcept {
    return entries_.size();
}

const ActionDefinition& ActionMap::definition(ActionId id) const {
    ENGINE_ASSERT(id < entries_.size(), "ActionMap::definition called with an unknown id");
    return entries_[id].definition;
}

const std::vector<Binding>& ActionMap::bindings(ActionId id) const {
    ENGINE_ASSERT(id < entries_.size(), "ActionMap::bindings called with an unknown id");
    const Entry& entry = entries_[id];
    return entry.override ? *entry.override : entry.definition.default_bindings;
}

bool ActionMap::has_override(ActionId id) const {
    ENGINE_ASSERT(id < entries_.size(), "ActionMap::has_override called with an unknown id");
    return entries_[id].override.has_value();
}

core::Result<core::Unit, ActionError> ActionMap::rebind(ActionId id, std::vector<Binding> replacement) {
    if (id >= entries_.size()) {
        return core::failure(make_error(ActionErrorCode::UnknownAction, "no action has this id"));
    }
    Entry& entry = entries_[id];
    if (auto problem = bindings_problem(entry.definition.name, replacement)) {
        return core::failure(make_error(ActionErrorCode::InvalidBinding, std::move(*problem)));
    }
    entry.override = std::move(replacement);
    return core::success();
}

core::Result<core::Unit, ActionError> ActionMap::reset(ActionId id) {
    if (id >= entries_.size()) {
        return core::failure(make_error(ActionErrorCode::UnknownAction, "no action has this id"));
    }
    entries_[id].override.reset();
    return core::success();
}

void ActionMap::reset_all() noexcept {
    for (Entry& entry : entries_) {
        entry.override.reset();
    }
}

ProfileBindings ActionMap::overrides() const {
    ProfileBindings saved;
    for (const Entry& entry : entries_) {
        if (entry.override) {
            saved.emplace(entry.definition.name, *entry.override);
        }
    }
    return saved;
}

core::Result<core::Unit, ActionError> ActionMap::apply_overrides(const ProfileBindings& profile) {
    // Check every entry before changing anything, so a bad profile leaves the current overrides in place.
    for (const auto& [name, entry_bindings] : profile) {
        if (!index_.contains(name)) {
            return core::failure(make_error(ActionErrorCode::UnknownAction, "profile names unknown action '" + name + "'"));
        }
        if (auto problem = bindings_problem(name, entry_bindings)) {
            return core::failure(make_error(ActionErrorCode::InvalidBinding, std::move(*problem)));
        }
    }

    for (Entry& entry : entries_) {
        entry.override.reset();
    }
    for (const auto& [name, entry_bindings] : profile) {
        entries_[index_.find(name)->second].override = entry_bindings;
    }
    return core::success();
}

void ActionEvaluator::update(const ActionMap& map, const InputState& input) {
    entries_.resize(map.size());
    for (ActionId id = 0; id < map.size(); ++id) {
        const ActionKind kind = map.definition(id).kind;
        float total = 0.0f;
        for (const Binding& binding : map.bindings(id)) {
            total += contribution(binding, input);
        }
        total = std::clamp(total, -1.0f, 1.0f);

        Entry& entry = entries_[id];
        const bool now_down = kind == ActionKind::Button ? total >= kButtonThreshold : total != 0.0f;
        entry.value = kind == ActionKind::Button ? (now_down ? 1.0f : 0.0f) : total;
        entry.pressed = now_down && !entry.down;
        entry.released = !now_down && entry.down;
        entry.down = now_down;
    }
}

float ActionEvaluator::value(ActionId id) const noexcept {
    return id < entries_.size() ? entries_[id].value : 0.0f;
}

bool ActionEvaluator::down(ActionId id) const noexcept {
    return id < entries_.size() && entries_[id].down;
}

bool ActionEvaluator::pressed(ActionId id) const noexcept {
    return id < entries_.size() && entries_[id].pressed;
}

bool ActionEvaluator::released(ActionId id) const noexcept {
    return id < entries_.size() && entries_[id].released;
}

}  // namespace engine::platform
