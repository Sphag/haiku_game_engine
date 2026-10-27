#pragma once

#include "engine/platform/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::platform {

// Turns the event stream into per-frame states: held, pressed this frame, and released this frame.
//
// Each frame: call begin_frame(), then apply() every event drained for that frame. Redundant events are rejected and
// counted, so a repeated press of a held key or a second release never creates a second edge. The backend uses the
// same rule before it queues an event, so the queue holds no duplicates either.
class InputState {
public:
    struct MouseDelta {
        std::int32_t dx = 0;
        std::int32_t dy = 0;
    };

    // Clears the press/release edges, the text, the mouse delta, and the wheel. Held states carry over.
    void begin_frame() noexcept;

    // Applies one event. Returns false, and changes nothing, for a redundant event: pressing a key that is already held,
    // releasing one that is up, or connecting a pad that is connected. Those are counted in duplicates_ignored().
    bool apply(const InputEvent& event);

    // Events that release every keyboard key and mouse button held by `window`, in a fixed order. Used when the window
    // loses focus. Gamepads are not included: they are polled, so they never go stale.
    [[nodiscard]] std::vector<InputEvent> release_held(WindowId window) const;

    [[nodiscard]] bool key_down(Key key) const noexcept;
    [[nodiscard]] bool key_pressed(Key key) const noexcept;
    [[nodiscard]] bool key_released(Key key) const noexcept;

    [[nodiscard]] bool mouse_down(MouseButton button) const noexcept;
    [[nodiscard]] bool mouse_pressed(MouseButton button) const noexcept;
    [[nodiscard]] bool mouse_released(MouseButton button) const noexcept;
    [[nodiscard]] MouseDelta mouse_delta() const noexcept;
    [[nodiscard]] double wheel_x() const noexcept;
    [[nodiscard]] double wheel_y() const noexcept;

    // UTF-8 text typed this frame, in order.
    [[nodiscard]] const std::string& text() const noexcept;

    [[nodiscard]] bool gamepad_connected(std::uint32_t slot) const noexcept;
    [[nodiscard]] bool gamepad_down(std::uint32_t slot, GamepadButton button) const noexcept;
    [[nodiscard]] bool gamepad_pressed(std::uint32_t slot, GamepadButton button) const noexcept;
    [[nodiscard]] bool gamepad_released(std::uint32_t slot, GamepadButton button) const noexcept;
    [[nodiscard]] float gamepad_axis(std::uint32_t slot, GamepadAxis axis) const noexcept;

    // Redundant events rejected since the last begin_frame().
    [[nodiscard]] std::size_t duplicates_ignored() const noexcept;

private:
    struct Gamepad {
        bool connected = false;
        std::array<bool, kGamepadButtonCount> down{};
        std::array<bool, kGamepadButtonCount> pressed{};
        std::array<bool, kGamepadButtonCount> released{};
        std::array<float, kGamepadAxisCount> axes{};
    };

    std::array<bool, kKeyCount> keys_down_{};
    std::array<bool, kKeyCount> keys_pressed_{};
    std::array<bool, kKeyCount> keys_released_{};
    std::array<bool, kMouseButtonCount> mouse_down_{};
    std::array<bool, kMouseButtonCount> mouse_pressed_{};
    std::array<bool, kMouseButtonCount> mouse_released_{};
    MouseDelta mouse_delta_;
    double wheel_x_ = 0.0;
    double wheel_y_ = 0.0;
    std::string text_;
    std::array<Gamepad, kMaxGamepads> gamepads_{};
    std::size_t duplicates_ = 0;
};

}  // namespace engine::platform
