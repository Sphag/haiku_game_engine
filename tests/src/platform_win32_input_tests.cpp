// Win32 input through the real window procedure (US-009). Keyboard and text messages are sent to a real window with
// SendMessage, so they run the same WndProc path as the OS. Raw Input reports cannot be produced without a device, so
// those are decoded from synthetic RAWMOUSE structs. Focus changes are sent as the messages the OS would send, as in
// platform_win32_tests.cpp. Real keyboard focus and a physical pad are not exercised here; they belong to the manual
// GUI check.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
#include "input_win32.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using engine::platform::GamepadConnected;
using engine::platform::InputEvent;
using engine::platform::KeyPressed;
using engine::platform::KeyReleased;
using engine::platform::Key;
using engine::platform::MouseButton;
using engine::platform::MouseButtonPressed;
using engine::platform::MouseButtonReleased;
using engine::platform::MouseMoved;
using engine::platform::MouseWheelScrolled;
using engine::platform::Platform;
using engine::platform::PlatformErrorCode;
using engine::platform::TextInput;
using engine::platform::WindowDesc;
using engine::platform::WindowId;

namespace {

// lParam layout for WM_KEYDOWN and WM_KEYUP: scan code in bits 16-23, extended in bit 24, repeat in bit 30.
constexpr LPARAM kKeyDownLParam = 0x001E0001;  // 'A' key, first press
constexpr LPARAM kKeyRepeatLParam = 0x401E0001;  // the same key while held: previous state was down
constexpr LPARAM kKeyUpLParam = static_cast<LPARAM>(0xC01E0001u);  // transition bit set on release

std::unique_ptr<Platform> make_platform() {
    auto created = Platform::create();
    REQUIRE(created.ok());
    return std::move(created).value();
}

HWND hwnd_of(const Platform& platform, WindowId id) {
    auto handles = platform.win32_handles(id);
    REQUIRE(handles.ok());
    return static_cast<HWND>(handles.value().hwnd);
}

// Keyboard and text events from the platform's queue, in order.
std::vector<InputEvent> drain_input(Platform& platform) {
    return platform.drain_input_events();
}

std::size_t count_key_events(const std::vector<InputEvent>& events, bool pressed) {
    std::size_t count = 0;
    for (const InputEvent& event : events) {
        if (pressed && std::holds_alternative<KeyPressed>(event.data)) {
            ++count;
        }
        if (!pressed && std::holds_alternative<KeyReleased>(event.data)) {
            ++count;
        }
    }
    return count;
}

// The clip rectangle the window's client area occupies on screen, which is what ClipCursor is set to while focused.
RECT client_screen_rect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    ClientToScreen(hwnd, &top_left);
    ClientToScreen(hwnd, &bottom_right);
    return RECT{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
}

bool same_rect(const RECT& lhs, const RECT& rhs) {
    return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

}  // namespace

TEST_CASE("WM_KEYDOWN and WM_KEYUP become key edges; auto-repeat adds nothing", "[integration]") {
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    const HWND hwnd = hwnd_of(*platform, id);
    drain_input(*platform);  // start from an empty queue

    SendMessageW(hwnd, WM_KEYDOWN, 'A', kKeyDownLParam);
    SendMessageW(hwnd, WM_KEYDOWN, 'A', kKeyRepeatLParam);
    SendMessageW(hwnd, WM_KEYDOWN, 'A', kKeyRepeatLParam);
    SendMessageW(hwnd, WM_KEYUP, 'A', kKeyUpLParam);

    const std::vector<InputEvent> events = drain_input(*platform);
    REQUIRE(events.size() == 2);
    REQUIRE(std::holds_alternative<KeyPressed>(events[0].data));
    CHECK(std::get<KeyPressed>(events[0].data).key == Key::A);
    REQUIRE(std::holds_alternative<KeyReleased>(events[1].data));
    CHECK(std::get<KeyReleased>(events[1].data).key == Key::A);
    CHECK(events[0].window == id);
    CHECK(events[1].window == id);
}

TEST_CASE("WM_CHAR delivers UTF-8 text, joins surrogate pairs, and drops control keys", "[integration]") {
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    const HWND hwnd = hwnd_of(*platform, id);
    drain_input(*platform);

    SendMessageW(hwnd, WM_CHAR, 0x0008, 0);                          // Backspace: control, dropped
    SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(0xD83D), 0);     // high surrogate of U+1F600
    SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(0xDE00), 0);     // low surrogate: one code point
    SendMessageW(hwnd, WM_CHAR, L'\r', 0);                           // Enter: control, dropped
    SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(L'é'), 0);  // e with acute

    std::string typed;
    for (const InputEvent& event : drain_input(*platform)) {
        if (const auto* text = std::get_if<TextInput>(&event.data)) {
            typed += text->utf8;
        }
    }
    CHECK(typed == std::string("\xF0\x9F\x98\x80\xC3\xA9"));
}

TEST_CASE("Mouse buttons and the wheel come from window messages, so the activating click is not lost", "[integration]") {
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    const HWND hwnd = hwnd_of(*platform, id);
    drain_input(*platform);

    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    SendMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(10, 10));
    SendMessageW(hwnd, WM_RBUTTONUP, 0, MAKELPARAM(10, 10));
    SendMessageW(hwnd, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), MAKELPARAM(10, 10));
    SendMessageW(hwnd, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), MAKELPARAM(10, 10));
    SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -2 * WHEEL_DELTA), MAKELPARAM(10, 10));
    SendMessageW(hwnd, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(10, 10));

    const std::vector<InputEvent> events = drain_input(*platform);
    REQUIRE(events.size() == 8);
    REQUIRE(std::holds_alternative<MouseButtonPressed>(events[0].data));
    CHECK(std::get<MouseButtonPressed>(events[0].data).button == MouseButton::Left);
    REQUIRE(std::holds_alternative<MouseButtonReleased>(events[1].data));
    CHECK(std::get<MouseButtonReleased>(events[1].data).button == MouseButton::Left);
    CHECK(std::get<MouseButtonPressed>(events[2].data).button == MouseButton::Right);
    CHECK(std::get<MouseButtonReleased>(events[3].data).button == MouseButton::Right);
    CHECK(std::get<MouseButtonPressed>(events[4].data).button == MouseButton::X2);
    CHECK(std::get<MouseButtonReleased>(events[5].data).button == MouseButton::X2);
    REQUIRE(std::holds_alternative<MouseWheelScrolled>(events[6].data));
    CHECK(std::get<MouseWheelScrolled>(events[6].data).notches_y == -2.0);
    CHECK(std::get<MouseWheelScrolled>(events[7].data).notches_x == 1.0);
    for (const InputEvent& event : events) {
        CHECK(event.window == id);
    }
}

TEST_CASE("Losing mouse capture while a button is held releases the button once", "[integration]") {
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    const HWND hwnd = hwnd_of(*platform, id);
    drain_input(*platform);

    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
    SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));  // the late release adds nothing

    std::size_t pressed = 0;
    std::size_t released = 0;
    for (const InputEvent& event : drain_input(*platform)) {
        pressed += std::holds_alternative<MouseButtonPressed>(event.data) ? 1 : 0;
        released += std::holds_alternative<MouseButtonReleased>(event.data) ? 1 : 0;
    }
    CHECK(pressed == 1);
    CHECK(released == 1);
}

TEST_CASE("Focus loss releases held keys once, and the later key-up adds no second release", "[integration]") {
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    const HWND hwnd = hwnd_of(*platform, id);
    drain_input(*platform);

    SendMessageW(hwnd, WM_KEYDOWN, 'W', 0x00110001);
    drain_input(*platform);  // W is held now

    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    const std::vector<InputEvent> released = drain_input(*platform);
    REQUIRE(released.size() == 1);
    REQUIRE(std::holds_alternative<KeyReleased>(released[0].data));
    CHECK(std::get<KeyReleased>(released[0].data).key == Key::W);
    CHECK(released[0].window == id);

    // The window never sees WM_KEYUP for a key it lost focus on; a late one must not add a second release.
    SendMessageW(hwnd, WM_KEYUP, 'W', kKeyUpLParam);
    CHECK(count_key_events(drain_input(*platform), false) == 0);
}

TEST_CASE("Raw mouse reports decode to relative motion, buttons, and wheel notches", "[unit]") {
    const WindowId id{7, 1};

    RAWMOUSE press{};
    press.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL;
    press.usButtonData = static_cast<USHORT>(static_cast<SHORT>(-2 * WHEEL_DELTA));
    press.lLastX = 5;
    press.lLastY = -3;
    const std::vector<InputEvent> events = engine::platform::win32::raw_mouse_events(press, id);
    REQUIRE(events.size() == 3);
    REQUIRE(std::holds_alternative<MouseButtonPressed>(events[0].data));
    CHECK(std::get<MouseButtonPressed>(events[0].data).button == MouseButton::Left);
    REQUIRE(std::holds_alternative<MouseWheelScrolled>(events[1].data));
    CHECK(std::get<MouseWheelScrolled>(events[1].data).notches_y == -2.0);
    REQUIRE(std::holds_alternative<MouseMoved>(events[2].data));
    CHECK(std::get<MouseMoved>(events[2].data).dx == 5);
    CHECK(std::get<MouseMoved>(events[2].data).dy == -3);
    for (const InputEvent& event : events) {
        CHECK(event.window == id);
    }

    RAWMOUSE horizontal{};
    horizontal.usButtonFlags = RI_MOUSE_HWHEEL;
    horizontal.usButtonData = static_cast<USHORT>(WHEEL_DELTA);
    const std::vector<InputEvent> tilt = engine::platform::win32::raw_mouse_events(horizontal, id);
    REQUIRE(tilt.size() == 1);
    CHECK(std::get<MouseWheelScrolled>(tilt[0].data).notches_x == 1.0);

    // Absolute reports (touch, remote desktop) carry positions, not deltas, so they produce no motion.
    RAWMOUSE absolute{};
    absolute.usFlags = MOUSE_MOVE_ABSOLUTE;
    absolute.lLastX = 100;
    absolute.lLastY = 200;
    CHECK(engine::platform::win32::raw_mouse_events(absolute, id).empty());
}

TEST_CASE("Right and side mouse buttons decode with their own press and release", "[unit]") {
    const WindowId id{7, 1};

    RAWMOUSE report{};
    report.usButtonFlags = RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_BUTTON_5_UP;
    const std::vector<InputEvent> events = engine::platform::win32::raw_mouse_events(report, id);
    REQUIRE(events.size() == 2);
    CHECK(std::get<MouseButtonPressed>(events[0].data).button == MouseButton::Right);
    CHECK(std::get<engine::platform::MouseButtonReleased>(events[1].data).button == MouseButton::X2);
}

TEST_CASE("Cursor capture is per window and clips the cursor only while focused", "[integration]") {
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    const HWND hwnd = hwnd_of(*platform, id);

    REQUIRE(platform->cursor_captured(id).value() == false);
    REQUIRE(platform->set_cursor_capture(id, true).ok());
    CHECK(platform->cursor_captured(id).value() == true);

    // Not focused yet: capture is requested, but the cursor is not clipped.
    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    RECT clip{};
    GetClipCursor(&clip);
    CHECK_FALSE(same_rect(clip, client_screen_rect(hwnd)));

    // Focus arrives: the clip is the client area in screen coordinates.
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    GetClipCursor(&clip);
    CHECK(same_rect(clip, client_screen_rect(hwnd)));

    // Turning capture off releases the clip.
    REQUIRE(platform->set_cursor_capture(id, false).ok());
    CHECK(platform->cursor_captured(id).value() == false);
    GetClipCursor(&clip);
    CHECK_FALSE(same_rect(clip, client_screen_rect(hwnd)));
}

TEST_CASE("Destroying a captured window releases the cursor clip", "[integration]") {
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    const HWND hwnd = hwnd_of(*platform, id);
    REQUIRE(platform->set_cursor_capture(id, true).ok());
    SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
    REQUIRE(platform->destroy_window(id).ok());
    platform->pump_events();

    RECT clip{};
    GetClipCursor(&clip);
    // With no clip the system reports the whole virtual screen, which is not a window's client area.
    CHECK(clip.right - clip.left > 0);
    CHECK(platform->set_cursor_capture(id, true).error().code == PlatformErrorCode::NotFound);
}

TEST_CASE("Gamepad polling runs on every pump and never fails the frame", "[integration]") {
    // Whether a controller is attached depends on the machine, so this test asserts only that polling completes and
    // that gamepad events always carry an invalid window id. Hot-plug transitions are covered by the diff tests in
    // platform_input_tests.cpp.
    auto platform = make_platform();
    const WindowId id = platform->create_window(WindowDesc{}).value();
    static_cast<void>(id);
    for (int frame = 0; frame < 3; ++frame) {
        platform->pump_events();
        for (const InputEvent& event : drain_input(*platform)) {
            const bool is_pad = std::holds_alternative<GamepadConnected>(event.data);
            if (is_pad) {
                CHECK_FALSE(event.window.valid());
            }
        }
    }
}

TEST_CASE("The window title and the pointer shape can be changed after creation", "[integration]") {
    auto platform = make_platform();
    WindowDesc desc;
    desc.title = "before";
    const WindowId id = platform->create_window(desc).value();
    const HWND hwnd = hwnd_of(*platform, id);

    REQUIRE(platform->set_window_title(id, "Редактор - main.scene *").ok());
    wchar_t text[64] = {};
    GetWindowTextW(hwnd, text, 64);
    CHECK(std::wstring(text) == L"Редактор - main.scene *");
    CHECK(platform->set_window_title(id, "Редактор - main.scene *").ok());  // unchanged: accepted, nothing sent

    // An embedded NUL is refused like at creation, and the old title stays.
    CHECK_FALSE(platform->set_window_title(id, std::string("a\0b", 3)).ok());
    GetWindowTextW(hwnd, text, 64);
    CHECK(std::wstring(text) == L"Редактор - main.scene *");

    // The OS asks for the cursor while the pointer is over the client area; the answer is the chosen shape.
    for (const auto& [shape, resource] : {std::pair<engine::platform::CursorShape, LPCWSTR>{engine::platform::CursorShape::Text, MAKEINTRESOURCEW(32513)},
                                          std::pair<engine::platform::CursorShape, LPCWSTR>{engine::platform::CursorShape::Hand, MAKEINTRESOURCEW(32649)},
                                          std::pair<engine::platform::CursorShape, LPCWSTR>{engine::platform::CursorShape::ResizeHorizontal, MAKEINTRESOURCEW(32644)},
                                          std::pair<engine::platform::CursorShape, LPCWSTR>{engine::platform::CursorShape::Arrow, MAKEINTRESOURCEW(32512)}}) {
        REQUIRE(platform->set_cursor_shape(id, shape).ok());
        const LRESULT handled = SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        CHECK(handled == TRUE);
        CHECK(GetCursor() == LoadCursorW(nullptr, resource));
    }

    CHECK(platform->destroy_window(id).ok());
    CHECK(platform->set_window_title(id, "gone").error().code == PlatformErrorCode::NotFound);
    CHECK(platform->set_cursor_shape(id, engine::platform::CursorShape::Text).error().code == PlatformErrorCode::NotFound);
}
