#pragma once

// Actions bind named game controls to device inputs, so controls can change without code (US-011). An action is a
// button or an axis. Its bindings read keys, mouse buttons, mouse motion and wheel, and gamepad buttons and axes.
// Several bindings can drive one action; they combine in the same frame. The map holds the authored defaults, and a
// user's remapping is an override per action that a profile can save (action_profile.hpp).
//
// The pure model here takes an InputState and builds on every OS.

#include "engine/core/result.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/input_state.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::platform {

// A device control that a binding reads. Gamepad sources name a slot, or read any connected gamepad when slot is
// nullopt.
struct KeySource {
    Key key = Key::Unknown;
};
struct MouseButtonSource {
    MouseButton button = MouseButton::Left;
};
struct MouseAxisSource {
    MouseAxis axis = MouseAxis::MotionX;
};
struct GamepadButtonSource {
    GamepadButton button = GamepadButton::A;
    std::optional<std::uint32_t> slot;
};
struct GamepadAxisSource {
    GamepadAxis axis = GamepadAxis::LeftX;
    std::optional<std::uint32_t> slot;
};

using SourceData = std::variant<KeySource, MouseButtonSource, MouseAxisSource, GamepadButtonSource, GamepadAxisSource>;

// One device control bound to an action. Its contribution per frame is the source value, minus the dead zone, times
// the scale. A key or button reads 1 when held and 0 otherwise. Mouse motion is in device counts and the wheel in
// notches, so their scale is a sensitivity. A scale of -1 makes a key or stick push negative, so a pair of keys forms
// an axis.
struct Binding {
    SourceData source;
    float scale = 1.0f;
    // Only gamepad axes take a dead zone, in [0, 1). Values inside it read 0. Outside it, the range is rescaled so
    // the value still reaches 1 at full deflection.
    float dead_zone = 0.0f;
};

// A button is down while its combined value is at least kButtonThreshold. An axis takes the combined value, clamped
// to [-1, 1].
enum class ActionKind : std::uint8_t { Button, Axis };
inline constexpr float kButtonThreshold = 0.5f;

struct ActionDefinition {
    std::string name;
    ActionKind kind = ActionKind::Button;
    std::vector<Binding> default_bindings;
};

enum class ActionErrorCode : std::uint8_t {
    EmptyName,
    DuplicateName,
    UnknownAction,
    InvalidBinding,
    MalformedProfile,
    UnsupportedVersion,
    Io,
};

struct ActionError {
    ActionErrorCode code = ActionErrorCode::InvalidBinding;
    std::string message;
};

// Binding overrides by action name, as saved in a user profile. An action with no entry keeps its defaults. An entry
// with no bindings unbinds the action.
using ProfileBindings = std::map<std::string, std::vector<Binding>, std::less<>>;

// Returns the dead zone with the rescale applied, for a stick value in [-1, 1]. The sign is kept.
[[nodiscard]] float apply_axis_dead_zone(float value, float dead_zone) noexcept;

using ActionId = std::uint32_t;

class ActionMap {
public:
    // Defines an action with its default bindings. Names are non-empty and unique, and every binding must be valid.
    [[nodiscard]] core::Result<ActionId, ActionError> add_action(std::string name, ActionKind kind,
                                                                 std::vector<Binding> default_bindings);

    [[nodiscard]] std::optional<ActionId> find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const ActionDefinition& definition(ActionId id) const;

    // The bindings in effect: the user's override when there is one, otherwise the defaults.
    [[nodiscard]] const std::vector<Binding>& bindings(ActionId id) const;
    [[nodiscard]] bool has_override(ActionId id) const;

    // Remaps one action for this user. It replaces the bindings as a whole, so an empty list unbinds the action.
    [[nodiscard]] core::Result<core::Unit, ActionError> rebind(ActionId id, std::vector<Binding> replacement);
    // Returns one action to its defaults.
    [[nodiscard]] core::Result<core::Unit, ActionError> reset(ActionId id);
    void reset_all() noexcept;

    // Every override, for saving. Defaults are not included.
    [[nodiscard]] ProfileBindings overrides() const;
    // Replaces all overrides with `profile`. Nothing changes if any name is unknown or any binding is invalid.
    [[nodiscard]] core::Result<core::Unit, ActionError> apply_overrides(const ProfileBindings& profile);

private:
    struct Entry {
        ActionDefinition definition;
        std::optional<std::vector<Binding>> override;
    };

    std::vector<Entry> entries_;
    std::map<std::string, ActionId, std::less<>> index_;
};

// Evaluates every action of a map against one frame of gameplay input. Call update() once per frame, after the input
// router has produced the gameplay state. Ids outside the map read as zero.
class ActionEvaluator {
public:
    void update(const ActionMap& map, const InputState& input);

    [[nodiscard]] float value(ActionId id) const noexcept;
    [[nodiscard]] bool down(ActionId id) const noexcept;
    [[nodiscard]] bool pressed(ActionId id) const noexcept;
    [[nodiscard]] bool released(ActionId id) const noexcept;

private:
    struct Entry {
        float value = 0.0f;
        bool down = false;
        bool pressed = false;
        bool released = false;
    };

    std::vector<Entry> entries_;
};

}  // namespace engine::platform
