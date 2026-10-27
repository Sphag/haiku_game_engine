#include "input_win32.hpp"

#include <Xinput.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::platform::win32 {

namespace {

constexpr std::int32_t kStickFullScale = 32767;

}  // namespace

std::vector<InputEvent> raw_mouse_events(const RAWMOUSE& mouse, WindowId window) {
    std::vector<InputEvent> events;

    struct RawButton {
        USHORT down;
        USHORT up;
        MouseButton button;
    };
    constexpr std::array<RawButton, 5> kRawButtons{{
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, MouseButton::Left},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, MouseButton::Right},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, MouseButton::Middle},
        {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, MouseButton::X1},
        {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, MouseButton::X2},
    }};
    const USHORT flags = mouse.usButtonFlags;
    for (const RawButton& raw : kRawButtons) {
        // A press and release inside one report (a fast click) yields both events, in that order.
        if ((flags & raw.down) != 0) {
            events.push_back(InputEvent{window, MouseButtonPressed{raw.button}});
        }
        if ((flags & raw.up) != 0) {
            events.push_back(InputEvent{window, MouseButtonReleased{raw.button}});
        }
    }

    // usButtonData holds the wheel delta as a signed value in WHEEL_DELTA units (120 per notch).
    const auto wheel_delta = static_cast<SHORT>(mouse.usButtonData);
    if ((flags & RI_MOUSE_WHEEL) != 0) {
        events.push_back(InputEvent{window, MouseWheelScrolled{0.0, static_cast<double>(wheel_delta) / WHEEL_DELTA}});
    }
    if ((flags & RI_MOUSE_HWHEEL) != 0) {
        events.push_back(InputEvent{window, MouseWheelScrolled{static_cast<double>(wheel_delta) / WHEEL_DELTA, 0.0}});
    }

    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (mouse.lLastX != 0 || mouse.lLastY != 0)) {
        events.push_back(InputEvent{window, MouseMoved{static_cast<std::int32_t>(mouse.lLastX),
                                                       static_cast<std::int32_t>(mouse.lLastY)}});
    }
    return events;
}

GamepadSnapshot read_xinput_pad(std::uint32_t slot) {
    GamepadSnapshot snapshot;
    XINPUT_STATE state{};
    // Any failure, including ERROR_DEVICE_NOT_CONNECTED, means the slot is empty for this poll.
    if (XInputGetState(static_cast<DWORD>(slot), &state) != ERROR_SUCCESS) {
        return snapshot;
    }
    snapshot.connected = true;

    // Each XINPUT_GAMEPAD_* bit maps to one pad button through the same table the Linux evdev path uses.
    for (std::uint32_t bit = 1; bit <= 0x8000U; bit <<= 1) {
        if (const auto button = gamepad_button_from_xinput(static_cast<std::uint16_t>(bit))) {
            snapshot.buttons[static_cast<std::size_t>(*button)] = (state.Gamepad.wButtons & bit) != 0;
        }
    }

    const auto axis = [](GamepadAxis value) { return static_cast<std::size_t>(value); };
    snapshot.axes[axis(GamepadAxis::LeftX)] = apply_stick_deadzone(
        state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, kStickFullScale);
    snapshot.axes[axis(GamepadAxis::LeftY)] = apply_stick_deadzone(
        state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, kStickFullScale);
    snapshot.axes[axis(GamepadAxis::RightX)] = apply_stick_deadzone(
        state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, kStickFullScale);
    snapshot.axes[axis(GamepadAxis::RightY)] = apply_stick_deadzone(
        state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, kStickFullScale);
    snapshot.axes[axis(GamepadAxis::LeftTrigger)] =
        apply_trigger_threshold(state.Gamepad.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    snapshot.axes[axis(GamepadAxis::RightTrigger)] =
        apply_trigger_threshold(state.Gamepad.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    return snapshot;
}

Key key_from_message(WPARAM wparam, LPARAM lparam) {
    const auto bits = static_cast<std::uint32_t>(lparam);
    auto virtual_key = static_cast<UINT>(wparam);
    const UINT scan_code = (bits >> 16) & 0xFFu;
    const bool extended = (bits & (1u << 24)) != 0;
    if (virtual_key == VK_SHIFT || virtual_key == VK_CONTROL || virtual_key == VK_MENU) {
        // MAPVK_VSC_TO_VK_EX takes the 0xE0 prefix for extended keys and returns VK_LSHIFT/VK_RSHIFT and so on.
        virtual_key = MapVirtualKeyW(extended ? (0xE000u | scan_code) : scan_code, MAPVK_VSC_TO_VK_EX);
    }
    return key_from_virtual_key(virtual_key);
}

}  // namespace engine::platform::win32
