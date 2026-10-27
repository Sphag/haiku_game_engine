#include "engine/platform/input_names.hpp"

#include <array>
#include <cstddef>

namespace engine::platform {
namespace {

// Each table lists the names in enum order, so name[index] is the name of static_cast<Enum>(index). The static_asserts
// keep a table in step with its enum when a value is added.

constexpr std::array<std::string_view, kKeyCount> kKeyNames{
    "",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W",
    "X", "Y", "Z",
    "Digit0", "Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Escape", "Tab", "Space", "Enter", "Backspace", "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
    "Left", "Right", "Up", "Down",
    "LeftShift", "RightShift", "LeftControl", "RightControl", "LeftAlt", "RightAlt", "LeftSuper", "RightSuper",
};

constexpr std::array<std::string_view, kMouseButtonCount> kMouseButtonNames{
    "Left", "Right", "Middle", "X1", "X2",
};

constexpr std::array<std::string_view, kMouseAxisCount> kMouseAxisNames{
    "MotionX", "MotionY", "WheelX", "WheelY",
};

constexpr std::array<std::string_view, kGamepadButtonCount> kGamepadButtonNames{
    "A", "B", "X", "Y",
    "LeftShoulder", "RightShoulder",
    "Back", "Start",
    "LeftThumb", "RightThumb",
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
};

constexpr std::array<std::string_view, kGamepadAxisCount> kGamepadAxisNames{
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger",
};

static_assert(kKeyNames.size() == static_cast<std::size_t>(Key::Count));
static_assert(kMouseButtonNames.size() == static_cast<std::size_t>(MouseButton::Count));
static_assert(kMouseAxisNames.size() == static_cast<std::size_t>(MouseAxis::Count));
static_assert(kGamepadButtonNames.size() == static_cast<std::size_t>(GamepadButton::Count));
static_assert(kGamepadAxisNames.size() == static_cast<std::size_t>(GamepadAxis::Count));

// Finds the index of `name` in `names`, starting at `first`. Index 0 is skipped for keys, whose entry is the empty
// Unknown name, so an empty name never matches.
template <typename Enum, std::size_t N>
std::optional<Enum> lookup(const std::array<std::string_view, N>& names, std::string_view name, std::size_t first) {
    for (std::size_t index = first; index < N; ++index) {
        if (names[index] == name) {
            return static_cast<Enum>(index);
        }
    }
    return std::nullopt;
}

}  // namespace

std::string_view key_name(Key key) noexcept {
    return kKeyNames[static_cast<std::size_t>(key)];
}

std::optional<Key> key_from_name(std::string_view name) noexcept {
    return lookup<Key>(kKeyNames, name, 1);
}

std::string_view mouse_button_name(MouseButton button) noexcept {
    return kMouseButtonNames[static_cast<std::size_t>(button)];
}

std::optional<MouseButton> mouse_button_from_name(std::string_view name) noexcept {
    return lookup<MouseButton>(kMouseButtonNames, name, 0);
}

std::string_view mouse_axis_name(MouseAxis axis) noexcept {
    return kMouseAxisNames[static_cast<std::size_t>(axis)];
}

std::optional<MouseAxis> mouse_axis_from_name(std::string_view name) noexcept {
    return lookup<MouseAxis>(kMouseAxisNames, name, 0);
}

std::string_view gamepad_button_name(GamepadButton button) noexcept {
    return kGamepadButtonNames[static_cast<std::size_t>(button)];
}

std::optional<GamepadButton> gamepad_button_from_name(std::string_view name) noexcept {
    return lookup<GamepadButton>(kGamepadButtonNames, name, 0);
}

std::string_view gamepad_axis_name(GamepadAxis axis) noexcept {
    return kGamepadAxisNames[static_cast<std::size_t>(axis)];
}

std::optional<GamepadAxis> gamepad_axis_from_name(std::string_view name) noexcept {
    return lookup<GamepadAxis>(kGamepadAxisNames, name, 0);
}

}  // namespace engine::platform
