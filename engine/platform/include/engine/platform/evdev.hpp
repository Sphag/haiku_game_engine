#pragma once

// Pure parts of the Linux evdev gamepad path (US-010). The Linux backend opens /dev/input/event* and reads records with
// read(2); everything here works on plain integers and bytes, so it builds and tests on every operating system.
//
// Codes come from linux/input-event-codes.h. The record layout is struct input_event on LP64 Linux: a 16-byte timeval,
// then u16 type, u16 code, and s32 value, 24 bytes in all.
#include "engine/platform/input.hpp"
#include "engine/platform/input_mapping.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::platform::evdev {

inline constexpr std::uint16_t kEvKey = 0x01;
inline constexpr std::uint16_t kEvAbs = 0x03;
inline constexpr std::size_t kRecordSize = 24;

inline constexpr std::uint16_t kAbsX = 0x00;
inline constexpr std::uint16_t kAbsY = 0x01;
inline constexpr std::uint16_t kAbsZ = 0x02;
inline constexpr std::uint16_t kAbsRX = 0x03;
inline constexpr std::uint16_t kAbsRY = 0x04;
inline constexpr std::uint16_t kAbsRZ = 0x05;
inline constexpr std::uint16_t kAbsHat0X = 0x10;
inline constexpr std::uint16_t kAbsHat0Y = 0x11;
inline constexpr std::uint16_t kAbsMax = 0x3F;

inline constexpr std::uint16_t kBtnSouth = 0x130;  // A on an Xbox-style pad
inline constexpr std::uint16_t kBtnThumbR = 0x13E;
inline constexpr std::uint16_t kKeyMax = 0x2FF;

// One record from a device node. The timestamp is dropped: the engine orders events by the frame they arrive in.
struct Event {
    std::uint16_t type = 0;
    std::uint16_t code = 0;
    std::int32_t value = 0;
};

// Parses whole records from raw bytes. A trailing partial record is ignored, so the caller can keep reading.
[[nodiscard]] std::vector<Event> decode_records(const std::byte* bytes, std::size_t size);

// Gamepad button for an EV_KEY code (BTN_SOUTH and up). Codes with no pad button, such as BTN_MODE, return nullopt.
[[nodiscard]] std::optional<GamepadButton> button_from_code(std::uint16_t code) noexcept;

// Stick or trigger axis for an EV_ABS code. Hat codes are buttons, not axes, so they return nullopt.
[[nodiscard]] std::optional<GamepadAxis> axis_from_code(std::uint16_t code) noexcept;

// Range reported by EVIOCGABS for one absolute axis.
struct AxisRange {
    std::int32_t minimum = 0;
    std::int32_t maximum = 0;
    std::int32_t flat = 0;
};

// Stick value in [-1, 1] with +Y up. evdev sticks have the centre at (min + max) / 2, and their dead zone is the larger
// of the device's own flat value and 24 percent of the half range (the XInput left-stick default). Y is inverted here
// because evdev reports +Y downward.
[[nodiscard]] float normalize_stick(std::int32_t raw, AxisRange range, bool invert) noexcept;

// Trigger value in [0, 1]. Values within 12 percent of the minimum are 0, and the rest is rescaled to reach 1.
[[nodiscard]] float normalize_trigger(std::int32_t raw, AxisRange range) noexcept;

// Keeps the state of one gamepad from its evdev records. Hat (D-pad) codes set the four D-pad buttons, so a hat that
// returns to centre releases them. Construction marks the pad connected: a decoder exists only for an open device.
// Ranges come from EVIOCGABS and must be set before the first axis record.
class PadDecoder {
public:
    PadDecoder();

    // Records one absolute axis range. Axes without a range read as 0 until one is set.
    void set_range(std::uint16_t code, AxisRange range);

    // Applies one record. Records that are not buttons or axes, such as EV_SYN, are ignored.
    void apply(const Event& event);

    // Called when the device is gone. The snapshot then reads as disconnected, and diff_gamepad releases its buttons.
    void disconnect() noexcept;

    [[nodiscard]] const GamepadSnapshot& snapshot() const noexcept { return snapshot_; }

private:
    GamepadSnapshot snapshot_;
    std::array<AxisRange, kAbsMax + 1> ranges_{};
    std::array<bool, kAbsMax + 1> has_range_{};
};

// True when a device's EV_KEY and EV_ABS bitmaps describe a gamepad: it has a pad button (BTN_SOUTH) and the left
// stick's X axis. Keyboards and mice are rejected. The bitmaps are little-endian arrays of u64 words, as EVIOCGBIT
// returns them, and are passed as byte pointers with their sizes.
[[nodiscard]] bool is_gamepad(const std::byte* key_bits, std::size_t key_size, const std::byte* abs_bits,
                              std::size_t abs_size) noexcept;

// Why a device node could not be opened, in terms the OS layer can fill in from errno.
enum class OpenFailure : std::uint8_t { PermissionDenied, Missing, Other };

// Explains an open failure with the fix for it. The message names the path. Permission failures name the input group,
// which is how most distributions grant evdev access.
[[nodiscard]] std::string describe_open_failure(std::string_view path, OpenFailure failure);

// Lowest slot that is not in use, or nullopt when all kMaxGamepads slots are taken.
[[nodiscard]] std::optional<std::uint32_t> free_slot(const std::array<bool, kMaxGamepads>& in_use) noexcept;

}  // namespace engine::platform::evdev
