// Linux evdev gamepad decoding (US-010). These tests feed synthetic records and bitmaps through the pure decoder, so they
// run on every operating system. The device open, ioctl, and hotplug path needs /dev/input and is covered by the Linux
// run notes in docs/input.md.
#include "engine/platform/evdev.hpp"
#include "engine/platform/input_mapping.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using engine::platform::GamepadAxis;
using engine::platform::GamepadButton;
using engine::platform::GamepadSnapshot;
namespace evdev = engine::platform::evdev;

namespace {

// Packs one struct input_event (LP64): timestamp (16 bytes, zero here), u16 type, u16 code, s32 value.
void append_record(std::vector<std::byte>& bytes, std::uint16_t type, std::uint16_t code, std::int32_t value) {
    std::array<std::byte, evdev::kRecordSize> record{};
    std::memcpy(record.data() + 16, &type, sizeof(type));
    std::memcpy(record.data() + 18, &code, sizeof(code));
    std::memcpy(record.data() + 20, &value, sizeof(value));
    bytes.insert(bytes.end(), record.begin(), record.end());
}

std::vector<std::byte> bitmap_with(std::size_t bytes, std::uint32_t bit) {
    std::vector<std::byte> map(bytes, std::byte{0});
    map[bit / 8] = static_cast<std::byte>(1U << (bit % 8));
    return map;
}

}  // namespace

TEST_CASE("evdev: records decode in order and a partial trailing record is ignored", "[unit]") {
    std::vector<std::byte> bytes;
    append_record(bytes, evdev::kEvKey, 0x130, 1);
    append_record(bytes, evdev::kEvAbs, evdev::kAbsX, -1234);
    bytes.push_back(std::byte{0xFF});  // half a record, as a short read can leave

    const auto events = evdev::decode_records(bytes.data(), bytes.size());
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == evdev::kEvKey);
    CHECK(events[0].code == 0x130);
    CHECK(events[0].value == 1);
    CHECK(events[1].type == evdev::kEvAbs);
    CHECK(events[1].code == evdev::kAbsX);
    CHECK(events[1].value == -1234);
    CHECK(evdev::decode_records(nullptr, 0).empty());
}

TEST_CASE("evdev: pad button and axis codes map to the engine's names", "[unit]") {
    using engine::platform::evdev::axis_from_code;
    using engine::platform::evdev::button_from_code;

    CHECK(button_from_code(0x130) == GamepadButton::A);
    CHECK(button_from_code(0x131) == GamepadButton::B);
    CHECK(button_from_code(0x133) == GamepadButton::X);
    CHECK(button_from_code(0x134) == GamepadButton::Y);
    CHECK(button_from_code(0x136) == GamepadButton::LeftShoulder);
    CHECK(button_from_code(0x13A) == GamepadButton::Back);
    CHECK(button_from_code(0x13B) == GamepadButton::Start);
    CHECK(button_from_code(0x13D) == GamepadButton::LeftThumb);
    CHECK(!button_from_code(0x13C));  // the guide button has no XInput counterpart in this engine

    CHECK(axis_from_code(evdev::kAbsX) == GamepadAxis::LeftX);
    CHECK(axis_from_code(evdev::kAbsY) == GamepadAxis::LeftY);
    CHECK(axis_from_code(evdev::kAbsRX) == GamepadAxis::RightX);
    CHECK(axis_from_code(evdev::kAbsRZ) == GamepadAxis::RightTrigger);
    CHECK(!axis_from_code(evdev::kAbsHat0X));  // hats are buttons
}

TEST_CASE("evdev: sticks normalize with a dead zone and Y points up", "[unit]") {
    const evdev::AxisRange range{-32768, 32767, 0};
    using Catch::Approx;

    CHECK(evdev::normalize_stick(0, range, false) == 0.0f);
    CHECK(evdev::normalize_stick(5000, range, false) == 0.0f);  // inside the dead zone
    CHECK(evdev::normalize_stick(32767, range, false) == Approx(1.0f));
    CHECK(evdev::normalize_stick(-32768, range, false) == Approx(-1.0f));
    CHECK(evdev::normalize_stick(32767, range, true) == Approx(-1.0f));  // evdev's +Y is down

    // A device's own flat value widens the dead zone when it is larger than the default.
    const evdev::AxisRange flat{-128, 127, 40};
    CHECK(evdev::normalize_stick(30, flat, false) == 0.0f);
    CHECK(evdev::normalize_stick(127, flat, false) == Approx(1.0f));

    // A zero-width range has no centre to measure from.
    CHECK(evdev::normalize_stick(10, evdev::AxisRange{5, 5, 0}, false) == 0.0f);
}

TEST_CASE("evdev: triggers rise from 0 past the threshold to 1", "[unit]") {
    const evdev::AxisRange range{0, 255, 0};
    CHECK(evdev::normalize_trigger(0, range) == 0.0f);
    CHECK(evdev::normalize_trigger(20, range) == 0.0f);  // about 8 percent, inside the threshold
    CHECK(evdev::normalize_trigger(255, range) == Catch::Approx(1.0f));
    const float middle = evdev::normalize_trigger(128, range);
    CHECK(middle > 0.4f);
    CHECK(middle < 0.6f);
}

TEST_CASE("evdev: the decoder keeps buttons, axes, and the D-pad hat in one snapshot", "[unit]") {
    evdev::PadDecoder decoder;
    CHECK(decoder.snapshot().connected);
    decoder.set_range(evdev::kAbsX, evdev::AxisRange{-32768, 32767, 0});
    decoder.set_range(evdev::kAbsZ, evdev::AxisRange{0, 255, 0});

    decoder.apply(evdev::Event{evdev::kEvKey, 0x130, 1});
    CHECK(decoder.snapshot().buttons[static_cast<std::size_t>(GamepadButton::A)]);
    decoder.apply(evdev::Event{evdev::kEvKey, 0x130, 0});
    CHECK_FALSE(decoder.snapshot().buttons[static_cast<std::size_t>(GamepadButton::A)]);

    decoder.apply(evdev::Event{evdev::kEvAbs, evdev::kAbsX, 32767});
    CHECK(decoder.snapshot().axes[static_cast<std::size_t>(GamepadAxis::LeftX)] > 0.99f);
    decoder.apply(evdev::Event{evdev::kEvAbs, evdev::kAbsZ, 255});
    CHECK(decoder.snapshot().axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] == Catch::Approx(1.0f));

    // Hat left is -1 on X; a return to centre releases it, and the other direction is untouched.
    decoder.apply(evdev::Event{evdev::kEvAbs, evdev::kAbsHat0X, -1});
    CHECK(decoder.snapshot().buttons[static_cast<std::size_t>(GamepadButton::DPadLeft)]);
    CHECK_FALSE(decoder.snapshot().buttons[static_cast<std::size_t>(GamepadButton::DPadRight)]);
    decoder.apply(evdev::Event{evdev::kEvAbs, evdev::kAbsHat0X, 0});
    CHECK_FALSE(decoder.snapshot().buttons[static_cast<std::size_t>(GamepadButton::DPadLeft)]);
    decoder.apply(evdev::Event{evdev::kEvAbs, evdev::kAbsHat0Y, -1});  // up is negative on evdev
    CHECK(decoder.snapshot().buttons[static_cast<std::size_t>(GamepadButton::DPadUp)]);

    // An axis without a range does not move, so a missing EVIOCGABS cannot produce garbage values.
    decoder.apply(evdev::Event{evdev::kEvAbs, evdev::kAbsRX, 30000});
    CHECK(decoder.snapshot().axes[static_cast<std::size_t>(GamepadAxis::RightX)] == 0.0f);

    decoder.disconnect();
    CHECK_FALSE(decoder.snapshot().connected);
}

TEST_CASE("evdev: a gamepad needs a pad button and a left stick; keyboards and mice are rejected", "[unit]") {
    const std::vector<std::byte> keys = bitmap_with(96, 0x130);
    const std::vector<std::byte> abs = bitmap_with(8, evdev::kAbsX);
    CHECK(evdev::is_gamepad(keys.data(), keys.size(), abs.data(), abs.size()));

    const std::vector<std::byte> keyboard_keys = bitmap_with(96, 30);  // KEY_A
    CHECK_FALSE(evdev::is_gamepad(keyboard_keys.data(), keyboard_keys.size(), abs.data(), abs.size()));

    const std::vector<std::byte> no_stick = bitmap_with(8, evdev::kAbsY);
    CHECK_FALSE(evdev::is_gamepad(keys.data(), keys.size(), no_stick.data(), no_stick.size()));
}

TEST_CASE("evdev: permission and missing-device failures explain the fix", "[unit]") {
    const std::string denied = evdev::describe_open_failure("/dev/input/event3", evdev::OpenFailure::PermissionDenied);
    CHECK(denied.find("/dev/input/event3") != std::string::npos);
    CHECK(denied.find("permission denied") != std::string::npos);
    CHECK(denied.find("'input' group") != std::string::npos);

    const std::string missing = evdev::describe_open_failure("/dev/input", evdev::OpenFailure::Missing);
    CHECK(missing.find("/dev/input") != std::string::npos);
    CHECK(missing.find("no evdev devices") != std::string::npos);

    const std::string other = evdev::describe_open_failure("/dev/input/event9", evdev::OpenFailure::Other);
    CHECK(other.find("skipped") != std::string::npos);
}

TEST_CASE("evdev: the first free slot is used, and a full table reports none", "[unit]") {
    CHECK(evdev::free_slot({false, false, false, false}) == 0u);
    CHECK(evdev::free_slot({true, true, false, false}) == 2u);
    CHECK(evdev::free_slot({true, true, true, true}) == std::nullopt);
}
