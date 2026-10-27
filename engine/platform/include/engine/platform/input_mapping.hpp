#pragma once

// Pure conversions used by the input backends. They take plain integers, so they build and test on every OS.
// The Win32 backend feeds them virtual-key codes, UTF-16 units, and XInput values.

#include "engine/platform/input.hpp"
#include "engine/platform/input_state.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::platform {

// Windows virtual-key code (winuser.h VK_*) to Key. Shift, Control, and Alt must already be split into their left
// and right codes (VK_LSHIFT and so on). Codes with no Key return Key::Unknown.
[[nodiscard]] Key key_from_virtual_key(std::uint32_t virtual_key) noexcept;

// Assembles code points from UTF-16 units, as WM_CHAR delivers them. A surrogate pair becomes one code point. A
// lone surrogate is dropped. Control characters (C0, DEL, C1) are dropped, so Backspace, Enter, and Tab never become
// text.
class Utf16Decoder {
public:
    // Returns a code point when `unit` completes one.
    [[nodiscard]] std::optional<char32_t> push(char16_t unit) noexcept;

    // Forgets a pending high surrogate, for example when the window loses focus.
    void reset() noexcept;

private:
    char16_t high_ = 0;
    bool pending_high_ = false;
};

// True for code points that are text: anything that is not a control character and not a surrogate.
[[nodiscard]] bool is_text_codepoint(char32_t codepoint) noexcept;

// Encodes one code point as UTF-8. Invalid code points (surrogates, values above U+10FFFF) encode as U+FFFD.
[[nodiscard]] std::string encode_utf8(char32_t codepoint);

// Stick value in [-1, 1] for a raw XInput-style axis. Values inside the dead zone are 0, and the range outside it is
// rescaled, so the output reaches +-1 at the full deflection.
[[nodiscard]] float apply_stick_deadzone(std::int32_t raw, std::int32_t dead_zone, std::int32_t full_scale) noexcept;

// Trigger value in [0, 1]. Values at or below the threshold are 0.
[[nodiscard]] float apply_trigger_threshold(std::uint8_t raw, std::uint8_t threshold) noexcept;

// The state of one controller at one poll. A disconnected controller is a snapshot with connected == false and
// everything else at rest.
struct GamepadSnapshot {
    bool connected = false;
    std::array<bool, kGamepadButtonCount> buttons{};
    std::array<float, kGamepadAxisCount> axes{};
};

// Appends the events that move `current` (the InputState) to `next`. A connection change comes first when a pad
// appears, and last when it goes, after its held buttons are released and its axes reset to 0. Nothing is appended
// when nothing changed, so a steady pad produces no events.
void diff_gamepad(std::uint32_t slot, const InputState& current, const GamepadSnapshot& next,
                  std::vector<InputEvent>& out);

// XInput wButtons bit (XINPUT_GAMEPAD_*) to a pad button. The digital D-pad bits are included. Returns nullopt for
// bits that are not buttons, such as the thumbstick clicks in a multi-bit mask.
[[nodiscard]] std::optional<GamepadButton> gamepad_button_from_xinput(std::uint16_t bit) noexcept;

// X11 keysym (the unshifted keysym for a key's position, from Xlib/keysymdef.h) to Key. Letters map to the same Key
// in either case. Codes with no Key return Key::Unknown.
[[nodiscard]] Key key_from_x11_keysym(std::uint32_t keysym) noexcept;

// The character a keysym types, for when no input method is open. Latin-1 keysyms and Unicode keysyms (0x01000000 plus
// the code point) map to their code point. Keysyms that type nothing return nullopt.
[[nodiscard]] std::optional<char32_t> codepoint_from_keysym(std::uint32_t keysym) noexcept;

// Mouse button for an X11 core button number. Buttons 4 to 7 are wheel steps, not buttons: see wheel_from_x11_button.
[[nodiscard]] std::optional<MouseButton> mouse_button_from_x11(std::uint32_t button) noexcept;

// Wheel step for an X11 button press (4 and 5 are vertical, 6 and 7 horizontal). A notch is 1.0, positive away from the
// user for vertical and to the right for horizontal.
[[nodiscard]] std::optional<MouseWheelScrolled> wheel_from_x11_button(std::uint32_t button) noexcept;

// Removes ASCII control characters and C1 controls (U+0080 to U+009F, as UTF-8) from UTF-8 text. Text from an input
// method may carry a newline or a tab, which never becomes TextInput.
[[nodiscard]] std::string drop_control_characters(std::string_view utf8);

}  // namespace engine::platform
