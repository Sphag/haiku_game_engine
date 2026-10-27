#pragma once

#include "engine/platform/window.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace engine::platform {

// Physical-layout keys as mapped from the platform's virtual-key codes. Left and right modifiers are separate keys.
enum class Key : std::uint8_t {
    Unknown,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Tab, Space, Enter, Backspace, Insert, Delete, Home, End, PageUp, PageDown,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt, LeftSuper, RightSuper,
    Count,
};

enum class MouseButton : std::uint8_t { Left, Right, Middle, X1, X2, Count };

// Mouse motion and wheel as analog sources for actions. Motion is in device counts, the wheel in notches.
enum class MouseAxis : std::uint8_t { MotionX, MotionY, WheelX, WheelY, Count };

enum class GamepadButton : std::uint8_t {
    A, B, X, Y,
    LeftShoulder, RightShoulder,
    Back, Start,
    LeftThumb, RightThumb,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    Count,
};

// Sticks are in [-1, 1] with +Y up. Triggers are in [0, 1].
enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

inline constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
inline constexpr std::size_t kMouseButtonCount = static_cast<std::size_t>(MouseButton::Count);
inline constexpr std::size_t kMouseAxisCount = static_cast<std::size_t>(MouseAxis::Count);
inline constexpr std::size_t kGamepadButtonCount = static_cast<std::size_t>(GamepadButton::Count);
inline constexpr std::size_t kGamepadAxisCount = static_cast<std::size_t>(GamepadAxis::Count);
// XInput supports four controllers. Slots are 0 to kMaxGamepads - 1.
inline constexpr std::uint32_t kMaxGamepads = 4;

struct KeyPressed {
    Key key = Key::Unknown;
};
struct KeyReleased {
    Key key = Key::Unknown;
};

// One character typed, as UTF-8. Control characters never produce this event.
struct TextInput {
    std::string utf8;
};

// Relative mouse motion in device counts (raw input), not screen pixels.
struct MouseMoved {
    std::int32_t dx = 0;
    std::int32_t dy = 0;
};
struct MouseButtonPressed {
    MouseButton button = MouseButton::Left;
};
struct MouseButtonReleased {
    MouseButton button = MouseButton::Left;
};

// Wheel motion in notches (one notch is 120 device units). Positive vertical is away from the user.
struct MouseWheelScrolled {
    double notches_x = 0.0;
    double notches_y = 0.0;
};

struct GamepadConnected {
    std::uint32_t slot = 0;
};
struct GamepadDisconnected {
    std::uint32_t slot = 0;
};
struct GamepadButtonPressed {
    std::uint32_t slot = 0;
    GamepadButton button = GamepadButton::A;
};
struct GamepadButtonReleased {
    std::uint32_t slot = 0;
    GamepadButton button = GamepadButton::A;
};
struct GamepadAxisChanged {
    std::uint32_t slot = 0;
    GamepadAxis axis = GamepadAxis::LeftX;
    float value = 0.0f;
};

using InputEventData =
    std::variant<KeyPressed, KeyReleased, TextInput, MouseMoved, MouseButtonPressed, MouseButtonReleased,
                 MouseWheelScrolled, GamepadConnected, GamepadDisconnected, GamepadButtonPressed,
                 GamepadButtonReleased, GamepadAxisChanged>;

// Keyboard, text, and mouse events carry the window that had focus. Gamepad events carry no window, so their
// window id is invalid.
struct InputEvent {
    WindowId window;
    InputEventData data;
};

}  // namespace engine::platform
