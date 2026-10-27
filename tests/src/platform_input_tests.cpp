// Input state, replay, and mapping (US-009). These are CPU-only tests and run on every operating system: they feed
// synthetic event frames through InputState and check the pure mapping functions. The Win32 message path is checked
// separately in platform_win32_input_tests.cpp.

#include "engine/platform/input.hpp"
#include "engine/platform/input_mapping.hpp"
#include "engine/platform/input_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using engine::platform::apply_stick_deadzone;
using engine::platform::apply_trigger_threshold;
using engine::platform::diff_gamepad;
using engine::platform::encode_utf8;
using engine::platform::GamepadAxis;
using engine::platform::GamepadAxisChanged;
using engine::platform::GamepadButton;
using engine::platform::GamepadButtonPressed;
using engine::platform::GamepadButtonReleased;
using engine::platform::GamepadConnected;
using engine::platform::GamepadDisconnected;
using engine::platform::GamepadSnapshot;
using engine::platform::InputEvent;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::key_from_virtual_key;
using engine::platform::KeyPressed;
using engine::platform::KeyReleased;
using engine::platform::MouseButton;
using engine::platform::MouseButtonPressed;
using engine::platform::MouseButtonReleased;
using engine::platform::MouseMoved;
using engine::platform::MouseWheelScrolled;
using engine::platform::TextInput;
using engine::platform::Utf16Decoder;
using engine::platform::WindowId;

namespace {

// One frame of a replay: the events drained during that frame.
using Frame = std::vector<InputEvent>;

const WindowId kWindow{1, 1};

InputEvent key_pressed(Key key) {
    return InputEvent{kWindow, KeyPressed{key}};
}

InputEvent key_released(Key key) {
    return InputEvent{kWindow, KeyReleased{key}};
}

// Runs one frame: begin_frame, then every event in order.
void play(InputState& state, const Frame& frame) {
    state.begin_frame();
    for (const InputEvent& event : frame) {
        state.apply(event);
    }
}

// Counts the events of type T in a list.
template <typename T>
std::size_t count_of(const std::vector<InputEvent>& events) {
    std::size_t count = 0;
    for (const InputEvent& event : events) {
        if (std::holds_alternative<T>(event.data)) {
            ++count;
        }
    }
    return count;
}

void apply_all(InputState& state, const std::vector<InputEvent>& events) {
    for (const InputEvent& event : events) {
        state.apply(event);
    }
}

}  // namespace

TEST_CASE("Replay: pressed, held, and released edges span frames", "[unit]") {
    // Frame 0 presses A. Frame 1 has no events, so A is held but not pressed. Frame 2 releases it.
    const std::vector<Frame> replay = {
        {key_pressed(Key::A)},
        {},
        {key_released(Key::A)},
    };
    InputState state;

    play(state, replay[0]);
    CHECK(state.key_pressed(Key::A));
    CHECK(state.key_down(Key::A));
    CHECK_FALSE(state.key_released(Key::A));

    play(state, replay[1]);
    CHECK_FALSE(state.key_pressed(Key::A));
    CHECK(state.key_down(Key::A));
    CHECK_FALSE(state.key_released(Key::A));

    play(state, replay[2]);
    CHECK_FALSE(state.key_pressed(Key::A));
    CHECK_FALSE(state.key_down(Key::A));
    CHECK(state.key_released(Key::A));
}

TEST_CASE("Replay: repeated presses in one frame produce one edge", "[unit]") {
    // Auto-repeat and a second WM_KEYDOWN both arrive as KeyPressed for a key that is already down.
    const std::vector<Frame> replay = {
        {key_pressed(Key::Space), key_pressed(Key::Space), key_pressed(Key::Space)},
        {key_pressed(Key::Space)},
    };
    InputState state;

    play(state, replay[0]);
    CHECK(state.key_pressed(Key::Space));
    CHECK(state.key_down(Key::Space));
    CHECK(state.duplicates_ignored() == 2);

    play(state, replay[1]);
    CHECK_FALSE(state.key_pressed(Key::Space));
    CHECK(state.key_down(Key::Space));
    CHECK(state.duplicates_ignored() == 1);
}

TEST_CASE("Replay: a release of a key that is up is ignored", "[unit]") {
    InputState state;
    state.begin_frame();
    CHECK_FALSE(state.apply(key_released(Key::B)));
    CHECK_FALSE(state.key_released(Key::B));
    CHECK(state.duplicates_ignored() == 1);
}

TEST_CASE("Replay: press and release in one frame keep both edges", "[unit]") {
    InputState state;
    play(state, {key_pressed(Key::Enter), key_released(Key::Enter)});
    CHECK(state.key_pressed(Key::Enter));
    CHECK(state.key_released(Key::Enter));
    CHECK_FALSE(state.key_down(Key::Enter));
}

TEST_CASE("Replay: press, release, press in one frame ends held", "[unit]") {
    InputState state;
    play(state, {key_pressed(Key::Z), key_released(Key::Z), key_pressed(Key::Z)});
    CHECK(state.key_pressed(Key::Z));
    CHECK(state.key_released(Key::Z));
    CHECK(state.key_down(Key::Z));
    CHECK(state.duplicates_ignored() == 0);
}

TEST_CASE("Unknown keys and invalid slots are rejected without changing state", "[unit]") {
    InputState state;
    state.begin_frame();
    CHECK_FALSE(state.apply(key_pressed(Key::Unknown)));
    CHECK_FALSE(state.apply(InputEvent{kWindow, GamepadConnected{engine::platform::kMaxGamepads}}));
    CHECK_FALSE(state.gamepad_connected(engine::platform::kMaxGamepads));
    CHECK(state.duplicates_ignored() == 0);
}

TEST_CASE("Focus loss releases exactly the held keys and buttons, in a fixed order", "[unit]") {
    InputState state;
    play(state, {key_pressed(Key::A), key_pressed(Key::LeftShift), key_pressed(Key::Tab),
                 InputEvent{kWindow, MouseButtonPressed{MouseButton::Left}}});
    // Releasing Tab by hand first leaves A, LeftShift, and the left button held.
    play(state, {key_released(Key::Tab)});

    const std::vector<InputEvent> released = state.release_held(kWindow);
    REQUIRE(released.size() == 3);
    REQUIRE(std::holds_alternative<KeyReleased>(released[0].data));
    CHECK(std::get<KeyReleased>(released[0].data).key == Key::A);
    REQUIRE(std::holds_alternative<KeyReleased>(released[1].data));
    CHECK(std::get<KeyReleased>(released[1].data).key == Key::LeftShift);
    REQUIRE(std::holds_alternative<MouseButtonReleased>(released[2].data));
    CHECK(std::get<MouseButtonReleased>(released[2].data).button == MouseButton::Left);
    for (const InputEvent& event : released) {
        CHECK(event.window == kWindow);
    }

    // Applying the releases clears the held states and produces release edges in the same frame.
    state.begin_frame();
    apply_all(state, released);
    CHECK_FALSE(state.key_down(Key::A));
    CHECK_FALSE(state.key_down(Key::LeftShift));
    CHECK_FALSE(state.mouse_down(MouseButton::Left));
    CHECK(state.key_released(Key::A));
    CHECK(state.key_released(Key::LeftShift));
    CHECK(state.mouse_released(MouseButton::Left));
    CHECK(state.release_held(kWindow).empty());
}

TEST_CASE("Unicode text arrives as UTF-8 in order, without control characters", "[unit]") {
    // Backspace (0x08) and Enter (0x0D) are control characters and must not appear. The rest is h, e with acute, a
    // CJK character, and an emoji that WM_CHAR delivers as the surrogate pair D83D DE00.
    const std::u16string typed = u"\u0008h\u000Dé世\xD83D\xDE00";

    Utf16Decoder decoder;
    InputState state;
    state.begin_frame();
    for (const char16_t unit : typed) {
        if (const auto codepoint = decoder.push(unit)) {
            state.apply(InputEvent{kWindow, TextInput{encode_utf8(*codepoint)}});
        }
    }
    CHECK(state.text() == std::string("h\xC3\xA9\xE4\xB8\x96\xF0\x9F\x98\x80"));
}

TEST_CASE("UTF-8 encoding covers one to four bytes and rejects invalid code points", "[unit]") {
    CHECK(encode_utf8(U'A') == "A");
    CHECK(encode_utf8(0xE9) == "\xC3\xA9");
    CHECK(encode_utf8(0x4E16) == "\xE4\xB8\x96");
    CHECK(encode_utf8(0x1F600) == "\xF0\x9F\x98\x80");
    CHECK(encode_utf8(0xD800) == "\xEF\xBF\xBD");   // surrogate: replacement character
    CHECK(encode_utf8(0x110000) == "\xEF\xBF\xBD");  // above U+10FFFF: replacement character
}

TEST_CASE("UTF-16 decoder joins surrogate pairs and drops lone surrogates", "[unit]") {
    Utf16Decoder decoder;

    CHECK_FALSE(decoder.push(0xDE00).has_value());  // a lone low surrogate is dropped
    CHECK_FALSE(decoder.push(0xD83D).has_value());  // a high surrogate waits for its pair
    CHECK(decoder.push(0xDE00) == std::optional<char32_t>{0x1F600});

    // A high surrogate followed by an ordinary unit drops the high and keeps the unit.
    CHECK_FALSE(decoder.push(0xD83D).has_value());
    CHECK(decoder.push(u'x') == std::optional<char32_t>{U'x'});

    // reset() forgets a pending high surrogate, for example after a focus change.
    CHECK_FALSE(decoder.push(0xD83D).has_value());
    decoder.reset();
    CHECK_FALSE(decoder.push(0xDE00).has_value());
}

TEST_CASE("Mouse motion and wheel accumulate within a frame and reset at the next", "[unit]") {
    InputState state;
    play(state, {InputEvent{kWindow, MouseMoved{3, -2}}, InputEvent{kWindow, MouseMoved{1, 5}},
                 InputEvent{kWindow, MouseWheelScrolled{0.0, 1.0}}, InputEvent{kWindow, MouseWheelScrolled{0.0, 0.5}}});
    CHECK(state.mouse_delta().dx == 4);
    CHECK(state.mouse_delta().dy == 3);
    CHECK(state.wheel_y() == 1.5);
    CHECK(state.wheel_x() == 0.0);

    state.begin_frame();
    CHECK(state.mouse_delta().dx == 0);
    CHECK(state.mouse_delta().dy == 0);
    CHECK(state.wheel_y() == 0.0);
    CHECK_FALSE(state.apply(InputEvent{kWindow, MouseMoved{0, 0}}));
}

TEST_CASE("Mouse buttons report press and release edges per frame", "[unit]") {
    InputState state;
    play(state, {InputEvent{kWindow, MouseButtonPressed{MouseButton::Right}}});
    CHECK(state.mouse_pressed(MouseButton::Right));
    CHECK(state.mouse_down(MouseButton::Right));

    play(state, {InputEvent{kWindow, MouseButtonReleased{MouseButton::Right}}});
    CHECK_FALSE(state.mouse_pressed(MouseButton::Right));
    CHECK(state.mouse_released(MouseButton::Right));
    CHECK_FALSE(state.mouse_down(MouseButton::Right));
}

TEST_CASE("Gamepad diff emits changes once and nothing for a steady pad", "[unit]") {
    InputState state;
    std::vector<InputEvent> out;

    GamepadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<std::size_t>(GamepadButton::A)] = true;
    pad.axes[static_cast<std::size_t>(GamepadAxis::LeftX)] = 0.5f;

    // First poll: connect, press A, move the stick.
    diff_gamepad(0, state, pad, out);
    REQUIRE(out.size() == 3);
    CHECK(std::holds_alternative<GamepadConnected>(out[0].data));
    CHECK(std::holds_alternative<GamepadButtonPressed>(out[1].data));
    CHECK(std::holds_alternative<GamepadAxisChanged>(out[2].data));
    apply_all(state, out);
    CHECK(state.gamepad_connected(0));
    CHECK(state.gamepad_down(0, GamepadButton::A));
    CHECK(state.gamepad_axis(0, GamepadAxis::LeftX) == 0.5f);

    // The same snapshot again is steady: no events, and nothing is queued twice.
    out.clear();
    diff_gamepad(0, state, pad, out);
    CHECK(out.empty());

    // Releasing A produces one release and nothing else.
    pad.buttons[static_cast<std::size_t>(GamepadButton::A)] = false;
    out.clear();
    diff_gamepad(0, state, pad, out);
    REQUIRE(out.size() == 1);
    REQUIRE(std::holds_alternative<GamepadButtonReleased>(out[0].data));
    CHECK(std::get<GamepadButtonReleased>(out[0].data).button == GamepadButton::A);
}

TEST_CASE("Unplugging a pad releases its buttons, zeroes its axes, then disconnects", "[unit]") {
    InputState state;
    GamepadSnapshot pad;
    pad.connected = true;
    pad.buttons[static_cast<std::size_t>(GamepadButton::Start)] = true;
    pad.axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] = 1.0f;
    std::vector<InputEvent> out;
    diff_gamepad(2, state, pad, out);
    apply_all(state, out);

    out.clear();
    diff_gamepad(2, state, GamepadSnapshot{}, out);
    REQUIRE(out.size() == 3);
    REQUIRE(std::holds_alternative<GamepadButtonReleased>(out[0].data));
    REQUIRE(std::holds_alternative<GamepadAxisChanged>(out[1].data));
    REQUIRE(std::holds_alternative<GamepadDisconnected>(out[2].data));
    CHECK(std::get<GamepadAxisChanged>(out[1].data).value == 0.0f);

    apply_all(state, out);
    CHECK_FALSE(state.gamepad_connected(2));
    CHECK_FALSE(state.gamepad_down(2, GamepadButton::Start));
    CHECK(state.gamepad_axis(2, GamepadAxis::RightTrigger) == 0.0f);

    // An empty slot stays silent on later polls.
    out.clear();
    diff_gamepad(2, state, GamepadSnapshot{}, out);
    CHECK(out.empty());
}

TEST_CASE("Stick dead zone and trigger threshold scale the range to [-1, 1] and [0, 1]", "[unit]") {
    constexpr std::int32_t kStickDeadZone = 7849;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    constexpr std::int32_t kFullScale = 32767;

    CHECK(apply_stick_deadzone(0, kStickDeadZone, kFullScale) == 0.0f);
    CHECK(apply_stick_deadzone(kStickDeadZone, kStickDeadZone, kFullScale) == 0.0f);
    CHECK(apply_stick_deadzone(-kStickDeadZone, kStickDeadZone, kFullScale) == 0.0f);
    CHECK(apply_stick_deadzone(kFullScale, kStickDeadZone, kFullScale) == 1.0f);
    CHECK(apply_stick_deadzone(-32768, kStickDeadZone, kFullScale) == -1.0f);

    // Halfway between the dead zone edge and full deflection is 0.5.
    const std::int32_t halfway = kStickDeadZone + (kFullScale - kStickDeadZone) / 2;
    CHECK(std::abs(apply_stick_deadzone(halfway, kStickDeadZone, kFullScale) - 0.5f) < 0.001f);
    CHECK(apply_stick_deadzone(-halfway, kStickDeadZone, kFullScale) < 0.0f);

    constexpr std::uint8_t kTriggerThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    CHECK(apply_trigger_threshold(0, kTriggerThreshold) == 0.0f);
    CHECK(apply_trigger_threshold(kTriggerThreshold, kTriggerThreshold) == 0.0f);
    CHECK(apply_trigger_threshold(255, kTriggerThreshold) == 1.0f);
    CHECK(std::abs(apply_trigger_threshold(142, kTriggerThreshold) - 0.5f) < 0.01f);
}

TEST_CASE("Virtual-key codes map to engine keys, and unmapped codes are Unknown", "[unit]") {
    CHECK(key_from_virtual_key(0x41) == Key::A);        // 'A'
    CHECK(key_from_virtual_key(0x5A) == Key::Z);        // 'Z'
    CHECK(key_from_virtual_key(0x30) == Key::Digit0);   // '0'
    CHECK(key_from_virtual_key(0x39) == Key::Digit9);   // '9'
    CHECK(key_from_virtual_key(0x70) == Key::F1);       // VK_F1
    CHECK(key_from_virtual_key(0x7B) == Key::F12);      // VK_F12
    CHECK(key_from_virtual_key(0x0D) == Key::Enter);    // VK_RETURN
    CHECK(key_from_virtual_key(0x1B) == Key::Escape);   // VK_ESCAPE
    CHECK(key_from_virtual_key(0x25) == Key::Left);     // VK_LEFT
    CHECK(key_from_virtual_key(0xA0) == Key::LeftShift);    // VK_LSHIFT
    CHECK(key_from_virtual_key(0xA5) == Key::RightAlt);     // VK_RMENU
    CHECK(key_from_virtual_key(0x5C) == Key::RightSuper);   // VK_RWIN
    CHECK(key_from_virtual_key(0x14) == Key::Unknown);  // VK_CAPITAL has no key in this table
    CHECK(key_from_virtual_key(0x07) == Key::Unknown);
}
