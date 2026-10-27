#include "engine/platform/evdev.hpp"
#include "engine/platform/input_mapping.hpp"

#include <algorithm>
#include <cstring>

namespace engine::platform::evdev {

namespace {

// linux/input-event-codes.h buttons for a gamepad, in the order XInput names them.
constexpr std::uint16_t kBtnEast = 0x131;   // B
constexpr std::uint16_t kBtnNorth = 0x133;  // X
constexpr std::uint16_t kBtnWest = 0x134;   // Y
constexpr std::uint16_t kBtnTl = 0x136;     // left shoulder
constexpr std::uint16_t kBtnTr = 0x137;     // right shoulder
constexpr std::uint16_t kBtnSelect = 0x13A;  // back
constexpr std::uint16_t kBtnStart = 0x13B;
constexpr std::uint16_t kBtnThumbL = 0x13D;

// Stick dead zone as a share of the half range: 7849 of 32767 for the left stick, the XInput default.
constexpr std::int64_t kStickDeadZonePercent = 24;
// Trigger dead zone: 30 of 255, the XInput default threshold.
constexpr float kTriggerThreshold = 0.12f;

std::int32_t read_i32(const std::byte* at) {
    std::int32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::uint16_t read_u16(const std::byte* at) {
    std::uint16_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

bool bit_is_set(const std::byte* bits, std::size_t size, std::uint32_t bit) noexcept {
    const std::size_t byte = bit / 8;
    if (byte >= size) {
        return false;
    }
    return (std::to_integer<unsigned>(bits[byte]) >> (bit % 8)) & 1U;
}

std::size_t axis_index(std::uint16_t code) noexcept {
    return static_cast<std::size_t>(code);
}

}  // namespace

std::vector<Event> decode_records(const std::byte* bytes, std::size_t size) {
    std::vector<Event> events;
    events.reserve(size / kRecordSize);
    for (std::size_t offset = 0; offset + kRecordSize <= size; offset += kRecordSize) {
        // The first 16 bytes are the timestamp, which is not used.
        const std::byte* record = bytes + offset;
        events.push_back(Event{read_u16(record + 16), read_u16(record + 18), read_i32(record + 20)});
    }
    return events;
}

std::optional<GamepadButton> button_from_code(std::uint16_t code) noexcept {
    switch (code) {
        case kBtnSouth: return GamepadButton::A;
        case kBtnEast: return GamepadButton::B;
        case kBtnNorth: return GamepadButton::X;
        case kBtnWest: return GamepadButton::Y;
        case kBtnTl: return GamepadButton::LeftShoulder;
        case kBtnTr: return GamepadButton::RightShoulder;
        case kBtnSelect: return GamepadButton::Back;
        case kBtnStart: return GamepadButton::Start;
        case kBtnThumbL: return GamepadButton::LeftThumb;
        case kBtnThumbR: return GamepadButton::RightThumb;
        default: return std::nullopt;
    }
}

std::optional<GamepadAxis> axis_from_code(std::uint16_t code) noexcept {
    switch (code) {
        case kAbsX: return GamepadAxis::LeftX;
        case kAbsY: return GamepadAxis::LeftY;
        case kAbsRX: return GamepadAxis::RightX;
        case kAbsRY: return GamepadAxis::RightY;
        case kAbsZ: return GamepadAxis::LeftTrigger;
        case kAbsRZ: return GamepadAxis::RightTrigger;
        default: return std::nullopt;
    }
}

float normalize_stick(std::int32_t raw, AxisRange range, bool invert) noexcept {
    const std::int64_t centre = (static_cast<std::int64_t>(range.minimum) + range.maximum) / 2;
    const std::int64_t half = (static_cast<std::int64_t>(range.maximum) - range.minimum) / 2;
    if (half <= 0) {
        return 0.0f;
    }
    const std::int64_t dead_zone = std::max<std::int64_t>(range.flat, half * kStickDeadZonePercent / 100);
    const float value = apply_stick_deadzone(static_cast<std::int32_t>(raw - centre),
                                             static_cast<std::int32_t>(dead_zone), static_cast<std::int32_t>(half));
    return invert ? -value : value;
}

float normalize_trigger(std::int32_t raw, AxisRange range) noexcept {
    const std::int64_t span = static_cast<std::int64_t>(range.maximum) - range.minimum;
    if (span <= 0) {
        return 0.0f;
    }
    const float fraction = static_cast<float>(raw - range.minimum) / static_cast<float>(span);
    if (fraction <= kTriggerThreshold) {
        return 0.0f;
    }
    const float value = (fraction - kTriggerThreshold) / (1.0f - kTriggerThreshold);
    return value > 1.0f ? 1.0f : value;
}

PadDecoder::PadDecoder() {
    // A decoder exists only for an open device, so the pad is connected from its first snapshot.
    snapshot_.connected = true;
}

void PadDecoder::set_range(std::uint16_t code, AxisRange range) {
    if (code > kAbsMax) {
        return;
    }
    ranges_[axis_index(code)] = range;
    has_range_[axis_index(code)] = true;
}

void PadDecoder::apply(const Event& event) {
    if (event.type == kEvKey) {
        // A value of 2 is the kernel's auto-repeat, which counts as held.
        if (const auto button = button_from_code(event.code)) {
            snapshot_.buttons[static_cast<std::size_t>(*button)] = event.value != 0;
        }
        return;
    }
    if (event.type != kEvAbs) {
        return;
    }

    // The hat reports -1, 0, or 1 on each axis. Left and up are negative.
    if (event.code == kAbsHat0X || event.code == kAbsHat0Y) {
        const bool negative_side = event.value < 0;
        const bool positive_side = event.value > 0;
        if (event.code == kAbsHat0X) {
            snapshot_.buttons[static_cast<std::size_t>(GamepadButton::DPadLeft)] = negative_side;
            snapshot_.buttons[static_cast<std::size_t>(GamepadButton::DPadRight)] = positive_side;
        } else {
            snapshot_.buttons[static_cast<std::size_t>(GamepadButton::DPadUp)] = negative_side;
            snapshot_.buttons[static_cast<std::size_t>(GamepadButton::DPadDown)] = positive_side;
        }
        return;
    }

    const auto axis = axis_from_code(event.code);
    if (!axis || !has_range_[axis_index(event.code)]) {
        return;
    }
    const AxisRange range = ranges_[axis_index(event.code)];
    const bool is_trigger = *axis == GamepadAxis::LeftTrigger || *axis == GamepadAxis::RightTrigger;
    const bool invert = *axis == GamepadAxis::LeftY || *axis == GamepadAxis::RightY;
    snapshot_.axes[static_cast<std::size_t>(*axis)] =
        is_trigger ? normalize_trigger(event.value, range) : normalize_stick(event.value, range, invert);
}

void PadDecoder::disconnect() noexcept {
    snapshot_.connected = false;
}

bool is_gamepad(const std::byte* key_bits, std::size_t key_size, const std::byte* abs_bits,
                std::size_t abs_size) noexcept {
    return bit_is_set(key_bits, key_size, kBtnSouth) && bit_is_set(abs_bits, abs_size, kAbsX);
}

std::string describe_open_failure(std::string_view path, OpenFailure failure) {
    const std::string name(path);
    switch (failure) {
        case OpenFailure::PermissionDenied:
            return "cannot read " + name +
                   " (permission denied). Add your user to the 'input' group, for example "
                   "'sudo usermod -aG input $USER', then log in again. Gamepads are not available until then.";
        case OpenFailure::Missing:
            return name +
                   " is missing: the system has no evdev devices, or the device was unplugged. WSL and some containers "
                   "have none, so gamepads are unavailable there.";
        case OpenFailure::Other:
            break;
    }
    return "cannot open " + name + "; the device is skipped";
}

std::optional<std::uint32_t> free_slot(const std::array<bool, kMaxGamepads>& in_use) noexcept {
    for (std::uint32_t slot = 0; slot < kMaxGamepads; ++slot) {
        if (!in_use[slot]) {
            return slot;
        }
    }
    return std::nullopt;
}

}  // namespace engine::platform::evdev
