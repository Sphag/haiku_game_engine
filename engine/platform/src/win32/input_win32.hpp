// Win32 input sources: Raw Input mouse reports and XInput controller state. These are the only OS-specific input
// pieces. Everything they return goes through the platform-neutral InputState and diff_gamepad.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "engine/platform/input.hpp"
#include "engine/platform/input_mapping.hpp"

#include <cstdint>
#include <vector>

namespace engine::platform::win32 {

// Translates one Raw Input mouse report into events for `window`. Absolute reports (touch screens, remote desktop)
// are ignored for motion, because their values are positions, not deltas.
[[nodiscard]] std::vector<InputEvent> raw_mouse_events(const RAWMOUSE& mouse, WindowId window);

// Reads one XInput controller. A slot with no controller returns a disconnected snapshot.
[[nodiscard]] GamepadSnapshot read_xinput_pad(std::uint32_t slot);

// Turns a virtual key from WM_KEYDOWN/WM_KEYUP into a Key. Shift, Control, and Alt arrive as generic codes, so their
// left or right side comes from the scan code and the extended-key bit.
[[nodiscard]] Key key_from_message(WPARAM wparam, LPARAM lparam);

}  // namespace engine::platform::win32
