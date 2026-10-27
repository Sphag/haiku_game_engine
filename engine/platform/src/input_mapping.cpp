#include "engine/platform/input_mapping.hpp"

namespace engine::platform {

namespace {

// winuser.h virtual-key codes used by the table below.
constexpr std::uint32_t kVkBack = 0x08;
constexpr std::uint32_t kVkTab = 0x09;
constexpr std::uint32_t kVkReturn = 0x0D;
constexpr std::uint32_t kVkShiftLeft = 0xA0;
constexpr std::uint32_t kVkShiftRight = 0xA1;
constexpr std::uint32_t kVkControlLeft = 0xA2;
constexpr std::uint32_t kVkControlRight = 0xA3;
constexpr std::uint32_t kVkMenuLeft = 0xA4;
constexpr std::uint32_t kVkMenuRight = 0xA5;
constexpr std::uint32_t kVkEscape = 0x1B;
constexpr std::uint32_t kVkSpace = 0x20;
constexpr std::uint32_t kVkPrior = 0x21;  // Page Up
constexpr std::uint32_t kVkNext = 0x22;   // Page Down
constexpr std::uint32_t kVkEnd = 0x23;
constexpr std::uint32_t kVkHome = 0x24;
constexpr std::uint32_t kVkLeft = 0x25;
constexpr std::uint32_t kVkUp = 0x26;
constexpr std::uint32_t kVkRight = 0x27;
constexpr std::uint32_t kVkDown = 0x28;
constexpr std::uint32_t kVkInsert = 0x2D;
constexpr std::uint32_t kVkDelete = 0x2E;
constexpr std::uint32_t kVkDigit0 = 0x30;
constexpr std::uint32_t kVkLetterA = 0x41;
constexpr std::uint32_t kVkLeftWindows = 0x5B;
constexpr std::uint32_t kVkRightWindows = 0x5C;
constexpr std::uint32_t kVkF1 = 0x70;

constexpr char32_t kReplacement = 0xFFFD;

constexpr bool is_high_surrogate(char16_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

constexpr bool is_low_surrogate(char16_t unit) noexcept {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

}  // namespace

Key key_from_virtual_key(std::uint32_t virtual_key) noexcept {
    if (virtual_key >= kVkLetterA && virtual_key < kVkLetterA + 26) {
        return static_cast<Key>(static_cast<std::uint32_t>(Key::A) + (virtual_key - kVkLetterA));
    }
    if (virtual_key >= kVkDigit0 && virtual_key < kVkDigit0 + 10) {
        return static_cast<Key>(static_cast<std::uint32_t>(Key::Digit0) + (virtual_key - kVkDigit0));
    }
    if (virtual_key >= kVkF1 && virtual_key < kVkF1 + 12) {
        return static_cast<Key>(static_cast<std::uint32_t>(Key::F1) + (virtual_key - kVkF1));
    }
    switch (virtual_key) {
        case kVkEscape:
            return Key::Escape;
        case kVkTab:
            return Key::Tab;
        case kVkSpace:
            return Key::Space;
        case kVkReturn:
            return Key::Enter;  // numpad Enter reports the same code
        case kVkBack:
            return Key::Backspace;
        case kVkInsert:
            return Key::Insert;
        case kVkDelete:
            return Key::Delete;
        case kVkHome:
            return Key::Home;
        case kVkEnd:
            return Key::End;
        case kVkPrior:
            return Key::PageUp;
        case kVkNext:
            return Key::PageDown;
        case kVkLeft:
            return Key::Left;
        case kVkRight:
            return Key::Right;
        case kVkUp:
            return Key::Up;
        case kVkDown:
            return Key::Down;
        case kVkShiftLeft:
            return Key::LeftShift;
        case kVkShiftRight:
            return Key::RightShift;
        case kVkControlLeft:
            return Key::LeftControl;
        case kVkControlRight:
            return Key::RightControl;
        case kVkMenuLeft:
            return Key::LeftAlt;
        case kVkMenuRight:
            return Key::RightAlt;
        case kVkLeftWindows:
            return Key::LeftSuper;
        case kVkRightWindows:
            return Key::RightSuper;
        default:
            return Key::Unknown;
    }
}

std::optional<char32_t> Utf16Decoder::push(char16_t unit) noexcept {
    if (pending_high_) {
        pending_high_ = false;
        if (is_low_surrogate(unit)) {
            const char32_t high = static_cast<char32_t>(high_ - 0xD800);
            const char32_t low = static_cast<char32_t>(unit - 0xDC00);
            return 0x10000 + ((high << 10) | low);
        }
        // An unpaired high surrogate is dropped. The unit that followed it is handled below.
    }
    if (is_high_surrogate(unit)) {
        high_ = unit;
        pending_high_ = true;
        return std::nullopt;
    }
    if (is_low_surrogate(unit)) {
        return std::nullopt;
    }
    const auto codepoint = static_cast<char32_t>(unit);
    if (!is_text_codepoint(codepoint)) {
        return std::nullopt;
    }
    return codepoint;
}

void Utf16Decoder::reset() noexcept {
    high_ = 0;
    pending_high_ = false;
}

bool is_text_codepoint(char32_t codepoint) noexcept {
    if (codepoint > 0x10FFFF) {
        return false;
    }
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F)) {
        return false;  // C0 controls, DEL, and C1 controls
    }
    return !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
}

std::string encode_utf8(char32_t codepoint) {
    const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
    if (surrogate || codepoint > 0x10FFFF) {
        codepoint = kReplacement;
    }
    std::string out;
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return out;
}

float apply_stick_deadzone(std::int32_t raw, std::int32_t dead_zone, std::int32_t full_scale) noexcept {
    if (full_scale <= dead_zone) {
        return 0.0f;
    }
    const auto wide = static_cast<std::int64_t>(raw);
    const std::int64_t magnitude = wide < 0 ? -wide : wide;
    if (magnitude <= dead_zone) {
        return 0.0f;
    }
    const auto span = static_cast<float>(full_scale - dead_zone);
    float value = static_cast<float>(magnitude - dead_zone) / span;
    if (value > 1.0f) {
        value = 1.0f;
    }
    return raw < 0 ? -value : value;
}

float apply_trigger_threshold(std::uint8_t raw, std::uint8_t threshold) noexcept {
    if (raw <= threshold) {
        return 0.0f;
    }
    return static_cast<float>(raw - threshold) / static_cast<float>(255 - threshold);
}

void diff_gamepad(std::uint32_t slot, const InputState& current, const GamepadSnapshot& next,
                  std::vector<InputEvent>& out) {
    if (slot >= kMaxGamepads) {
        return;
    }
    const bool was_connected = current.gamepad_connected(slot);
    if (!next.connected) {
        if (!was_connected) {
            return;
        }
        // Release everything the pad was holding before the disconnect, so no held state outlives the device.
        for (std::size_t i = 0; i < kGamepadButtonCount; ++i) {
            const auto button = static_cast<GamepadButton>(i);
            if (current.gamepad_down(slot, button)) {
                out.push_back(InputEvent{WindowId{}, GamepadButtonReleased{slot, button}});
            }
        }
        for (std::size_t i = 0; i < kGamepadAxisCount; ++i) {
            const auto axis = static_cast<GamepadAxis>(i);
            if (current.gamepad_axis(slot, axis) != 0.0f) {
                out.push_back(InputEvent{WindowId{}, GamepadAxisChanged{slot, axis, 0.0f}});
            }
        }
        out.push_back(InputEvent{WindowId{}, GamepadDisconnected{slot}});
        return;
    }

    if (!was_connected) {
        out.push_back(InputEvent{WindowId{}, GamepadConnected{slot}});
    }
    for (std::size_t i = 0; i < kGamepadButtonCount; ++i) {
        const auto button = static_cast<GamepadButton>(i);
        const bool was_down = current.gamepad_down(slot, button);
        if (next.buttons[i] && !was_down) {
            out.push_back(InputEvent{WindowId{}, GamepadButtonPressed{slot, button}});
        } else if (!next.buttons[i] && was_down) {
            out.push_back(InputEvent{WindowId{}, GamepadButtonReleased{slot, button}});
        }
    }
    for (std::size_t i = 0; i < kGamepadAxisCount; ++i) {
        const auto axis = static_cast<GamepadAxis>(i);
        if (next.axes[i] != current.gamepad_axis(slot, axis)) {
            out.push_back(InputEvent{WindowId{}, GamepadAxisChanged{slot, axis, next.axes[i]}});
        }
    }
}

std::optional<GamepadButton> gamepad_button_from_xinput(std::uint16_t bit) noexcept {
    switch (bit) {
        case 0x0001: return GamepadButton::DPadUp;
        case 0x0002: return GamepadButton::DPadDown;
        case 0x0004: return GamepadButton::DPadLeft;
        case 0x0008: return GamepadButton::DPadRight;
        case 0x0010: return GamepadButton::Start;
        case 0x0020: return GamepadButton::Back;
        case 0x0040: return GamepadButton::LeftThumb;
        case 0x0080: return GamepadButton::RightThumb;
        case 0x0100: return GamepadButton::LeftShoulder;
        case 0x0200: return GamepadButton::RightShoulder;
        case 0x1000: return GamepadButton::A;
        case 0x2000: return GamepadButton::B;
        case 0x4000: return GamepadButton::X;
        case 0x8000: return GamepadButton::Y;
        default: return std::nullopt;
    }
}

Key key_from_x11_keysym(std::uint32_t keysym) noexcept {
    // Keysyms from keysymdef.h. Letters arrive lowercase from an unshifted lookup, and uppercase keysyms are accepted too.
    if (keysym >= 0x61 && keysym <= 0x7A) {
        return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(keysym - 0x61));
    }
    if (keysym >= 0x41 && keysym <= 0x5A) {
        return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(keysym - 0x41));
    }
    if (keysym >= 0x30 && keysym <= 0x39) {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(keysym - 0x30));
    }
    if (keysym >= 0xFFBE && keysym <= 0xFFC9) {  // XK_F1 to XK_F12
        return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(keysym - 0xFFBE));
    }
    switch (keysym) {
        case 0x20: return Key::Space;
        case 0xFF1B: return Key::Escape;
        case 0xFF09: return Key::Tab;
        case 0xFF0D: return Key::Enter;
        case 0xFF08: return Key::Backspace;
        case 0xFF63: return Key::Insert;
        case 0xFFFF: return Key::Delete;
        case 0xFF50: return Key::Home;
        case 0xFF57: return Key::End;
        case 0xFF55: return Key::PageUp;
        case 0xFF56: return Key::PageDown;
        case 0xFF51: return Key::Left;
        case 0xFF52: return Key::Up;
        case 0xFF53: return Key::Right;
        case 0xFF54: return Key::Down;
        case 0xFFE1: return Key::LeftShift;
        case 0xFFE2: return Key::RightShift;
        case 0xFFE3: return Key::LeftControl;
        case 0xFFE4: return Key::RightControl;
        case 0xFFE9: return Key::LeftAlt;   // XK_Alt_L
        case 0xFFEA: return Key::RightAlt;  // XK_Alt_R
        case 0xFFEB: return Key::LeftSuper;
        case 0xFFEC: return Key::RightSuper;
        default: return Key::Unknown;
    }
}

std::optional<char32_t> codepoint_from_keysym(std::uint32_t keysym) noexcept {
    // Latin-1 keysyms share their code points. Unicode keysyms carry 0x01000000 plus the code point.
    if ((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF)) {
        return static_cast<char32_t>(keysym);
    }
    constexpr std::uint32_t kUnicodeBase = 0x01000000;
    if (keysym >= kUnicodeBase + 0x20 && keysym <= kUnicodeBase + 0x10FFFF) {
        const auto codepoint = static_cast<char32_t>(keysym - kUnicodeBase);
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return std::nullopt;
        }
        return codepoint;
    }
    return std::nullopt;
}

std::optional<MouseButton> mouse_button_from_x11(std::uint32_t button) noexcept {
    switch (button) {
        case 1: return MouseButton::Left;
        case 2: return MouseButton::Middle;
        case 3: return MouseButton::Right;
        case 8: return MouseButton::X1;
        case 9: return MouseButton::X2;
        default: return std::nullopt;
    }
}

std::optional<MouseWheelScrolled> wheel_from_x11_button(std::uint32_t button) noexcept {
    switch (button) {
        case 4: return MouseWheelScrolled{0.0, 1.0};   // wheel up, away from the user
        case 5: return MouseWheelScrolled{0.0, -1.0};  // wheel down
        case 6: return MouseWheelScrolled{-1.0, 0.0};  // tilt left
        case 7: return MouseWheelScrolled{1.0, 0.0};   // tilt right
        default: return std::nullopt;
    }
}

std::string drop_control_characters(std::string_view utf8) {
    std::string kept;
    kept.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        if (byte < 0x20 || byte == 0x7F) {
            continue;
        }
        // C1 controls are U+0080 to U+009F, encoded as 0xC2 0x80 to 0xC2 0x9F.
        if (byte == 0xC2 && i + 1 < utf8.size()) {
            const auto next = static_cast<unsigned char>(utf8[i + 1]);
            if (next >= 0x80 && next <= 0x9F) {
                ++i;
                continue;
            }
        }
        kept.push_back(utf8[i]);
    }
    return kept;
}

}  // namespace engine::platform
