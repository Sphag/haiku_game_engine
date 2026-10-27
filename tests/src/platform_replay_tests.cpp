// Shared input replay (US-010). tests/fixtures/input/shared_replay.txt holds one physical input sequence written with the
// native codes of both backends. Each line runs through the pure translation of one backend (Win32 virtual keys,
// UTF-16 units, XInput bits, and wheel deltas on Windows; X11 keysyms, buttons, and evdev codes on Linux), then through
// the same InputState and diff_gamepad that the backends use. Both backends must produce the expected normalized
// events, and the held state after the replay must match. These tests build and run on every operating system.
#include "engine/platform/evdev.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/input_mapping.hpp"
#include "engine/platform/input_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using engine::platform::diff_gamepad;
using engine::platform::drop_control_characters;
using engine::platform::encode_utf8;
using engine::platform::GamepadButton;
using engine::platform::GamepadConnected;
using engine::platform::GamepadDisconnected;
using engine::platform::GamepadButtonPressed;
using engine::platform::GamepadButtonReleased;
using engine::platform::GamepadSnapshot;
using engine::platform::InputEvent;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::key_from_virtual_key;
using engine::platform::key_from_x11_keysym;
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
namespace evdev = engine::platform::evdev;

namespace {

// One line of the fixture: its kind, its native fields, and the expected events.
struct Line {
    std::string kind;
    std::map<std::string, std::string> fields;
    std::vector<std::string> flags;
    std::string expected;
};

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(text);
    while (std::getline(stream, current, separator)) {
        parts.push_back(current);
    }
    return parts;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

std::vector<Line> read_fixture() {
    std::ifstream file(std::string(ENGINE_TEST_FIXTURE_DIR) + "/input/shared_replay.txt");
    REQUIRE(file.good());
    std::vector<Line> lines;
    std::string raw;
    while (std::getline(file, raw)) {
        const std::string text = trim(raw);
        if (text.empty() || text[0] == '#') {
            continue;
        }
        Line line;
        const auto bar = text.find('|');
        const std::string body = trim(bar == std::string::npos ? text : text.substr(0, bar));
        if (bar != std::string::npos) {
            line.expected = trim(text.substr(bar + 1));
        }
        std::istringstream fields(body);
        std::string token;
        fields >> line.kind;
        while (fields >> token) {
            const auto equals = token.find('=');
            if (equals == std::string::npos) {
                line.flags.push_back(token);
            } else {
                line.fields[token.substr(0, equals)] = token.substr(equals + 1);
            }
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

std::uint32_t hex(const std::string& value) {
    return static_cast<std::uint32_t>(std::stoul(value, nullptr, 0));
}

// Name of a normalized key or button, as the fixture writes it.
std::string key_name(Key key) {
    switch (key) {
        case Key::A: return "A";
        case Key::Space: return "Space";
        case Key::Enter: return "Enter";
        case Key::LeftShift: return "LeftShift";
        default: return "Key" + std::to_string(static_cast<int>(key));
    }
}

std::string button_name(GamepadButton button) {
    switch (button) {
        case GamepadButton::A: return "A";
        case GamepadButton::LeftShoulder: return "LeftShoulder";
        case GamepadButton::DPadLeft: return "DPadLeft";
        default: return "Button" + std::to_string(static_cast<int>(button));
    }
}

// The fixture's spelling of one normalized event. Every event kind the replay produces has a case.
std::string describe(const InputEvent& event) {
    return std::visit(
        [](const auto& data) -> std::string {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, KeyPressed>) {
                return "KeyPressed " + key_name(data.key);
            } else if constexpr (std::is_same_v<T, KeyReleased>) {
                return "KeyReleased " + key_name(data.key);
            } else if constexpr (std::is_same_v<T, TextInput>) {
                return "TextInput " + data.utf8;
            } else if constexpr (std::is_same_v<T, MouseMoved>) {
                return "MouseMoved " + std::to_string(data.dx) + " " + std::to_string(data.dy);
            } else if constexpr (std::is_same_v<T, MouseButtonPressed>) {
                return "MouseButtonPressed " + std::to_string(static_cast<int>(data.button));
            } else if constexpr (std::is_same_v<T, MouseButtonReleased>) {
                return "MouseButtonReleased " + std::to_string(static_cast<int>(data.button));
            } else if constexpr (std::is_same_v<T, MouseWheelScrolled>) {
                std::ostringstream out;
                out << "MouseWheelScrolled " << data.notches_x << " " << data.notches_y;
                return out.str();
            } else if constexpr (std::is_same_v<T, GamepadConnected>) {
                return "GamepadConnected " + std::to_string(data.slot);
            } else if constexpr (std::is_same_v<T, GamepadDisconnected>) {
                return "GamepadDisconnected " + std::to_string(data.slot);
            } else if constexpr (std::is_same_v<T, GamepadButtonPressed>) {
                return "GamepadButtonPressed " + std::to_string(data.slot) + " " + button_name(data.button);
            } else if constexpr (std::is_same_v<T, GamepadButtonReleased>) {
                return "GamepadButtonReleased " + std::to_string(data.slot) + " " + button_name(data.button);
            } else {
                return "GamepadAxisChanged " + std::to_string(data.slot);
            }
        },
        event.data);
}

// Turns one line into the candidate events for one backend. `state` is the replay's InputState before the line; a pad
// line diffs against it, as the backend does. The driver applies each candidate through the same InputState, so a
// redundant event is dropped by the same rule on every backend.
using Translate = std::function<std::vector<InputEvent>(const Line&, const InputState&)>;

struct ReplayResult {
    std::vector<std::string> lines;  // accepted events of each fixture line, joined with " ; "
    std::vector<std::string> held;   // keys held after the replay
};

ReplayResult run(const std::vector<Line>& fixture, const Translate& translate) {
    const WindowId window{1, 1};
    InputState state;
    ReplayResult result;
    for (const Line& line : fixture) {
        if (line.kind == "frame") {
            // Frames keep one (empty) result line, so result.lines lines up with the fixture's line numbers.
            state.begin_frame();
            result.lines.emplace_back();
            continue;
        }
        std::vector<std::string> accepted;
        // Focus loss is the backend's own release of everything held, so it is not translated from a native code.
        std::vector<InputEvent> candidates =
            line.kind == "focus-loss" ? state.release_held(window) : translate(line, state);
        for (InputEvent& event : candidates) {
            if (state.apply(event)) {
                accepted.push_back(describe(event));
            }
        }
        std::string joined;
        for (const std::string& text : accepted) {
            joined += (joined.empty() ? "" : " ; ") + text;
        }
        result.lines.push_back(joined);
    }
    for (std::uint32_t key = 0; key < static_cast<std::uint32_t>(Key::Count); ++key) {
        const auto normalized = static_cast<Key>(key);
        if (state.key_down(normalized)) {
            result.held.push_back(key_name(normalized));
        }
    }
    return result;
}

// Win32 path: virtual keys, UTF-16 units, XInput bits, and WHEEL_DELTA values. The Raw Input mouse and the XInput pad
// state are the other half of this path; the pure parts are what the fixture checks.
std::vector<InputEvent> translate_win32(const Line& line, const InputState& state, Utf16Decoder& decoder,
                                        GamepadSnapshot& pad) {
    const WindowId window{1, 1};
    std::vector<InputEvent> out;
    if (line.kind == "key") {
        const Key key = key_from_virtual_key(hex(line.fields.at("win")));
        const bool down = line.flags.at(0) == "down";
        if (down) {
            out.push_back(InputEvent{window, KeyPressed{key}});
        } else {
            out.push_back(InputEvent{window, KeyReleased{key}});
        }
    } else if (line.kind == "text") {
        std::string text;
        for (const std::string& unit : split(line.fields.at("win"), ',')) {
            if (const auto codepoint = decoder.push(static_cast<char16_t>(hex(unit)))) {
                text += encode_utf8(*codepoint);
            }
        }
        if (!text.empty()) {
            out.push_back(InputEvent{window, TextInput{drop_control_characters(text)}});
        }
    } else if (line.kind == "pad") {
        const auto button =
            engine::platform::gamepad_button_from_xinput(static_cast<std::uint16_t>(hex(line.fields.at("xinput"))));
        const bool down = line.flags.at(0) == "down";
        pad.connected = true;
        if (button) {
            pad.buttons[static_cast<std::size_t>(*button)] = down;
        }
        diff_gamepad(0, state, pad, out);
    } else if (line.kind == "wheel") {
        // WHEEL_DELTA is 120 per notch. WM_MOUSEHWHEEL carries the horizontal steps (axis=h in the fixture).
        const double notches = static_cast<double>(std::stoi(line.fields.at("win"))) / 120.0;
        const bool horizontal = line.fields.count("axis") != 0 && line.fields.at("axis") == "h";
        out.push_back(InputEvent{window, horizontal ? MouseWheelScrolled{notches, 0.0} : MouseWheelScrolled{0.0, notches}});
    } else if (line.kind == "mouse") {
        out.push_back(InputEvent{window, MouseMoved{std::stoi(line.fields.at("dx")), std::stoi(line.fields.at("dy"))}});
    }
    return out;
}

// Linux path: X11 keysyms, X11 buttons, evdev records, and the same diff_gamepad.
std::vector<InputEvent> translate_linux(const Line& line, const InputState& state, evdev::PadDecoder& decoder) {
    const WindowId window{1, 1};
    std::vector<InputEvent> out;
    if (line.kind == "key") {
        const Key key = key_from_x11_keysym(hex(line.fields.at("x11")));
        if (line.flags.at(0) == "down") {
            out.push_back(InputEvent{window, KeyPressed{key}});
        } else {
            out.push_back(InputEvent{window, KeyReleased{key}});
        }
    } else if (line.kind == "text") {
        const auto codepoint = engine::platform::codepoint_from_keysym(hex(line.fields.at("x11")));
        if (codepoint && engine::platform::is_text_codepoint(*codepoint)) {
            out.push_back(InputEvent{window, TextInput{encode_utf8(*codepoint)}});
        }
    } else if (line.kind == "pad") {
        const auto code = static_cast<std::uint16_t>(hex(line.fields.at("evdev")));
        const bool down = line.flags.at(0) == "down";
        decoder.apply(evdev::Event{evdev::kEvKey, code, down ? 1 : 0});
        diff_gamepad(0, state, decoder.snapshot(), out);
    } else if (line.kind == "wheel") {
        if (const auto wheel = engine::platform::wheel_from_x11_button(hex(line.fields.at("x11")))) {
            out.push_back(InputEvent{window, *wheel});
        }
    } else if (line.kind == "mouse") {
        out.push_back(InputEvent{window, MouseMoved{std::stoi(line.fields.at("dx")), std::stoi(line.fields.at("dy"))}});
    }
    return out;
}

}  // namespace

TEST_CASE("Replay: the shared fixture gives the same events on the Win32 and Linux translations", "[unit]") {
    const std::vector<Line> fixture = read_fixture();
    REQUIRE(!fixture.empty());

    Utf16Decoder decoder;
    GamepadSnapshot win_pad;
    const ReplayResult win = run(fixture, [&](const Line& line, const InputState& state) {
        return translate_win32(line, state, decoder, win_pad);
    });

    evdev::PadDecoder linux_pad;
    const ReplayResult linux = run(fixture, [&](const Line& line, const InputState& state) {
        return translate_linux(line, state, linux_pad);
    });

    REQUIRE(win.lines.size() == linux.lines.size());
    std::size_t checked = 0;
    for (std::size_t i = 0; i < fixture.size(); ++i) {
        if (fixture[i].kind == "frame") {
            continue;
        }
        INFO("fixture line " << i + 1 << ": " << fixture[i].kind);
        CHECK(win.lines[i] == fixture[i].expected);
        CHECK(linux.lines[i] == fixture[i].expected);
        ++checked;
    }
    CHECK(checked > 0);
    CHECK(win.held == linux.held);
    CHECK(win.held.empty());
}

TEST_CASE("Replay: a repeated press in one frame is one edge, and focus loss releases", "[unit]") {
    const std::vector<Line> fixture = read_fixture();
    Utf16Decoder decoder;
    GamepadSnapshot win_pad;
    const ReplayResult win = run(fixture, [&](const Line& line, const InputState& state) {
        return translate_win32(line, state, decoder, win_pad);
    });

    // The auto-repeat line (a second press with A still held) produces no event.
    bool saw_repeat = false;
    for (std::size_t i = 0; i < fixture.size(); ++i) {
        if (fixture[i].kind == "key" && fixture[i].flags.at(0) == "down" && fixture[i].expected.empty()) {
            saw_repeat = true;
            CHECK(win.lines[i].empty());
        }
    }
    CHECK(saw_repeat);
}

TEST_CASE("Mapping: X11 keysyms map to the same keys as Win32 virtual keys for Latin letters", "[unit]") {
    CHECK(key_from_x11_keysym(0x61) == Key::A);
    CHECK(key_from_x11_keysym(0x41) == Key::A);
    CHECK(key_from_x11_keysym(0x41) == key_from_virtual_key(0x41));
    CHECK(key_from_x11_keysym(0x30) == key_from_virtual_key(0x30));
    CHECK(key_from_x11_keysym(0xFFBE) == key_from_virtual_key(0x70));  // F1
    CHECK(key_from_x11_keysym(0xFFE1) == Key::LeftShift);
    CHECK(key_from_x11_keysym(0xFFE2) == Key::RightShift);
    CHECK(key_from_x11_keysym(0xFFE3) == Key::LeftControl);
    CHECK(key_from_x11_keysym(0xFFEA) == Key::RightAlt);
    CHECK(key_from_x11_keysym(0xFF0D) == Key::Enter);
    CHECK(key_from_x11_keysym(0x1000444) == Key::Unknown);  // a Cyrillic keysym has no Key
}

TEST_CASE("Mapping: keysyms type characters only when the keysym is text", "[unit]") {
    using engine::platform::codepoint_from_keysym;
    CHECK(codepoint_from_keysym(0x61) == U'a');
    CHECK(codepoint_from_keysym(0xFC) == U'ü');
    CHECK(codepoint_from_keysym(0x0101F600) == U'\U0001F600');
    CHECK(!codepoint_from_keysym(0xFF0D));  // Return types nothing; the newline is not text
    CHECK(!codepoint_from_keysym(0xFFE1));  // Shift types nothing
    CHECK(!codepoint_from_keysym(0x1000D800));  // a surrogate is not a character
}

TEST_CASE("Mapping: X11 buttons and wheel steps", "[unit]") {
    CHECK(engine::platform::mouse_button_from_x11(1) == MouseButton::Left);
    CHECK(engine::platform::mouse_button_from_x11(2) == MouseButton::Middle);
    CHECK(engine::platform::mouse_button_from_x11(3) == MouseButton::Right);
    CHECK(engine::platform::mouse_button_from_x11(8) == MouseButton::X1);
    CHECK(engine::platform::mouse_button_from_x11(9) == MouseButton::X2);
    CHECK(!engine::platform::mouse_button_from_x11(4));  // wheel up is a step, not a button
    CHECK(!engine::platform::wheel_from_x11_button(1));

    const auto up = engine::platform::wheel_from_x11_button(4);
    REQUIRE(up);
    CHECK(up->notches_y == 1.0);
    const auto left = engine::platform::wheel_from_x11_button(6);
    REQUIRE(left);
    CHECK(left->notches_x == -1.0);
}

TEST_CASE("Mapping: XInput bits map to the pad buttons the evdev path uses", "[unit]") {
    CHECK(engine::platform::gamepad_button_from_xinput(0x1000) == GamepadButton::A);
    CHECK(engine::platform::gamepad_button_from_xinput(0x8000) == GamepadButton::Y);
    CHECK(engine::platform::gamepad_button_from_xinput(0x0001) == GamepadButton::DPadUp);
    CHECK(engine::platform::gamepad_button_from_xinput(0x0010) == GamepadButton::Start);
    CHECK(!engine::platform::gamepad_button_from_xinput(0x0300));  // two bits are not one button
    CHECK(!engine::platform::gamepad_button_from_xinput(0x0000));
}

TEST_CASE("Mapping: control characters never become text", "[unit]") {
    CHECK(drop_control_characters("a\nb\tc") == "abc");
    CHECK(drop_control_characters("\r") == "");
    CHECK(drop_control_characters("\x7F" "x") == "x");
    CHECK(drop_control_characters("\xC2\x85" "y") == "y");  // U+0085 NEL, a C1 control
    CHECK(drop_control_characters("\xC3\xBC") == "\xC3\xBC");  // ü is kept
}
