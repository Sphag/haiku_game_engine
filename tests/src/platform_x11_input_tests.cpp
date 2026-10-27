// X11 input against a real X server (US-010). XTest injects keys, buttons, and relative motion through the server, so the
// same path a physical device takes is exercised: core key and button events, XIM text, and XInput2 raw motion. These
// tests need DISPLAY to point at a running server and a focused window. Without a server they SKIP. When the window
// manager does not give the test window keyboard focus, they SKIP too, because input is delivered to the focused window
// only. Gamepads need /dev/input, which WSL and most containers lack, so the gamepad test checks the no-device path.
#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"

#include "xlib_abi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using engine::platform::GamepadConnected;
using engine::platform::InputEvent;
using engine::platform::Key;
using engine::platform::KeyPressed;
using engine::platform::KeyReleased;
using engine::platform::MouseButton;
using engine::platform::MouseButtonPressed;
using engine::platform::MouseButtonReleased;
using engine::platform::MouseMoved;
using engine::platform::MouseWheelScrolled;
using engine::platform::Platform;
using engine::platform::TextInput;
using engine::platform::WindowDesc;
using engine::platform::WindowId;
namespace x11 = engine::platform::x11;

namespace {

// XTest entry points from libXtst.so.6. Bound at run time, like the Xlib subset, so the tests build without its headers.
struct XTest {
    int (*FakeKeyEvent)(x11::Display*, unsigned int, x11::Bool, unsigned long) = nullptr;
    int (*FakeButtonEvent)(x11::Display*, unsigned int, x11::Bool, unsigned long) = nullptr;
    int (*FakeRelativeMotionEvent)(x11::Display*, int, int, unsigned long) = nullptr;
};

// Binds one symbol, copying the address through memcpy as xlib_abi.cpp does.
template <typename Fn>
bool bind_symbol(void* library, const char* name, Fn& out) {
    void* symbol = dlsym(library, name);
    if (symbol == nullptr) {
        return false;
    }
    std::memcpy(&out, &symbol, sizeof(out));
    return true;
}

std::optional<XTest> load_xtest() {
    void* library = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        return std::nullopt;
    }
    XTest test;
    if (!bind_symbol(library, "XTestFakeKeyEvent", test.FakeKeyEvent) ||
        !bind_symbol(library, "XTestFakeButtonEvent", test.FakeButtonEvent) ||
        !bind_symbol(library, "XTestFakeRelativeMotionEvent", test.FakeRelativeMotionEvent)) {
        return std::nullopt;
    }
    return test;
}

// The part of XWindowAttributes the viewability check reads. Xlib writes the whole struct, so the layout is complete
// (it mirrors the one in platform_x11_tests.cpp).
struct XWindowAttributesLayout {
    int x;
    int y;
    int width;
    int height;
    int border_width;
    int depth;
    void* visual;
    unsigned long root;
    int window_class;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    int save_under;
    unsigned long colormap;
    int map_installed;
    int map_state;
    long all_event_masks;
    long your_event_mask;
    long do_not_propagate_mask;
    int override_redirect;
    void* screen;
};
constexpr int kIsViewable = 2;

using GetWindowAttributesFn = int (*)(x11::Display*, x11::Window, XWindowAttributesLayout*);
std::optional<GetWindowAttributesFn> load_get_window_attributes() {
    void* library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    GetWindowAttributesFn function = nullptr;
    if (library == nullptr || !bind_symbol(library, "XGetWindowAttributes", function)) {
        return std::nullopt;
    }
    return function;
}

// XSetInputFocus is not in the backend's Xlib subset, so the focus-loss test binds it from libX11 directly.
using SetInputFocusFn = int (*)(x11::Display*, x11::Window, int, x11::Time);
std::optional<SetInputFocusFn> load_set_input_focus() {
    void* library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    SetInputFocusFn function = nullptr;
    if (library == nullptr || !bind_symbol(library, "XSetInputFocus", function)) {
        return std::nullopt;
    }
    return function;
}

// XWarpPointer, bound from libX11. Core button events go to the window under the pointer, so the tests move the
// pointer into their window first.
using WarpPointerFn = int (*)(x11::Display*, x11::Window, x11::Window, int, int, unsigned int, unsigned int, int, int);
std::optional<WarpPointerFn> load_warp_pointer() {
    void* library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    WarpPointerFn function = nullptr;
    if (library == nullptr || !bind_symbol(library, "XWarpPointer", function)) {
        return std::nullopt;
    }
    return function;
}

// XQueryPointer, bound from libX11, so a test can confirm where the server put the pointer.
using QueryPointerFn = int (*)(x11::Display*, x11::Window, x11::Window*, x11::Window*, int*, int*, int*, int*,
                               unsigned int*);
std::optional<QueryPointerFn> load_query_pointer() {
    void* library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    QueryPointerFn function = nullptr;
    if (library == nullptr || !bind_symbol(library, "XQueryPointer", function)) {
        return std::nullopt;
    }
    return function;
}

// One Platform with one focused window, and the Xlib handles the tests inject through.
struct Session {
    std::unique_ptr<Platform> platform;
    WindowId window;
    x11::Display* display = nullptr;
    const x11::Api* api = nullptr;
    XTest test;
};

// Opens a session, or returns the reason it cannot run (no display, no XTest, or no keyboard focus).
std::optional<Session> open_focused_session(std::string& skip_reason) {
    const char* load_error = nullptr;
    const x11::Api* api = x11::load_api(&load_error);
    if (api == nullptr) {
        skip_reason = std::string("no X11 client library: ") + load_error;
        return std::nullopt;
    }
    auto created = Platform::create();
    if (!created.ok()) {
        skip_reason = "no X display: " + created.error().detail;
        return std::nullopt;
    }
    Session session;
    session.platform = std::move(created.value());
    session.api = api;

    auto test = load_xtest();
    if (!test) {
        skip_reason = "libXtst.so.6 is not installed, so input cannot be injected";
        return std::nullopt;
    }
    session.test = *test;

    WindowDesc desc;
    desc.title = "input test";
    desc.size = engine::platform::LogicalSize{320.0, 240.0};
    auto window = session.platform->create_window(desc);
    REQUIRE(window.ok());
    session.window = window.value();

    const auto handles = session.platform->xlib_handles(session.window);
    REQUIRE(handles.ok());
    session.display = static_cast<x11::Display*>(handles.value().display);

    // Keyboard events go to the focused window. The server refuses focus for a window that is not viewable, so the test
    // waits for the window manager to show it first. A refused request is never repeated while the window is hidden:
    // repeated invalid requests were followed by the X connection dropping during a run.
    const auto set_focus = load_set_input_focus();
    const auto get_attributes = load_get_window_attributes();
    if (!set_focus || !get_attributes) {
        skip_reason = "XSetInputFocus or XGetWindowAttributes is not available in libX11";
        return std::nullopt;
    }
    int map_state = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        XWindowAttributesLayout attributes{};
        if ((*get_attributes)(session.display, handles.value().window, &attributes) != 0) {
            map_state = attributes.map_state;
        }
        if (map_state == kIsViewable) {
            break;
        }
        session.platform->pump_events();
        static_cast<void>(session.platform->drain_events());
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (map_state != kIsViewable) {
        skip_reason = "the window manager did not show the window (map_state " + std::to_string(map_state) + ")";
        return std::nullopt;
    }
    for (int attempt = 0; attempt < 25; ++attempt) {
        static_cast<void>((*set_focus)(session.display, handles.value().window, 2 /* RevertToParent */, 0));
        session.api->XFlush(session.display);
        session.platform->pump_events();
        static_cast<void>(session.platform->drain_events());
        if (session.platform->window_info(session.window).value().focused) {
            // Pointer events go to the window under the pointer, so put the pointer over the test window.
            if (const auto warp = load_warp_pointer()) {
                static_cast<void>((*warp)(session.display, 0, handles.value().window, 0, 0, 0, 0, 20, 20));
                session.api->XFlush(session.display);
            }
            return session;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    skip_reason = "the server did not give the test window keyboard focus";
    return std::nullopt;
}

// Pumps for a while and returns every input event the server delivered. The server delivers injected input asynchronously.
std::vector<InputEvent> collect(Session& session, int milliseconds = 400) {
    std::vector<InputEvent> collected;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        session.platform->pump_events();
        for (InputEvent& event : session.platform->drain_input_events()) {
            collected.push_back(std::move(event));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return collected;
}

// Moves the pointer to the centre of the test window and waits until the server reports it there. XWayland applies warps
// asynchronously, so the position is polled (bounded, 1 s) rather than assumed. Relative motion and clicks are then
// injected from a known place, not from wherever an earlier test left the pointer.
void centre_pointer(Session& session) {
    const auto warp = load_warp_pointer();
    const auto query = load_query_pointer();
    if (!warp || !query) {
        return;
    }
    const auto handles = session.platform->xlib_handles(session.window);
    const x11::Window window = handles.value().window;
    const auto size = session.platform->window_info(session.window).value().framebuffer_size;
    const int centre_x = static_cast<int>(size.width / 2);
    const int centre_y = static_cast<int>(size.height / 2);
    for (int attempt = 0; attempt < 50; ++attempt) {
        static_cast<void>((*warp)(session.display, 0, window, 0, 0, 0, 0, centre_x, centre_y));
        session.api->XFlush(session.display);
        x11::Window root = 0;
        x11::Window child = 0;
        int root_x = 0;
        int root_y = 0;
        int win_x = 0;
        int win_y = 0;
        unsigned int mask = 0;
        if ((*query)(session.display, window, &root, &child, &root_x, &root_y, &win_x, &win_y, &mask) != 0 &&
            win_x >= 0 && win_y >= 0 && win_x < static_cast<int>(size.width) && win_y < static_cast<int>(size.height)) {
            // XWayland can deliver motion for positions the pointer left before the warp. Drain that before the test
            // injects its own motion, so the first delta is measured from the centre.
            static_cast<void>(collect(session, 150));
            return;
        }
        static_cast<void>(collect(session, 20));
    }
}

bool has_key(const std::vector<InputEvent>& events, Key key, bool pressed) {
    for (const InputEvent& event : events) {
        if (pressed) {
            if (const auto* press = std::get_if<KeyPressed>(&event.data); press != nullptr && press->key == key) {
                return true;
            }
        } else if (const auto* release = std::get_if<KeyReleased>(&event.data);
                   release != nullptr && release->key == key) {
            return true;
        }
    }
    return false;
}

// Total relative motion in device counts.
std::pair<std::int32_t, std::int32_t> total_motion(const std::vector<InputEvent>& events) {
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    for (const InputEvent& event : events) {
        if (const auto* motion = std::get_if<MouseMoved>(&event.data)) {
            dx += motion->dx;
            dy += motion->dy;
        }
    }
    return {dx, dy};
}

}  // namespace

TEST_CASE("X11 input: a key press reaches the focused window as KeyPressed, TextInput, and KeyReleased", "[integration]") {
    std::string skip_reason;
    auto session = open_focused_session(skip_reason);
    if (!session) {
        SKIP(skip_reason);
    }
    const x11::KeyCode code = static_cast<x11::KeyCode>(session->api->XKeysymToKeycode(session->display, 0x61));
    if (code == 0) {
        SKIP("the server keymap has no keycode for 'a'");
    }

    session->test.FakeKeyEvent(session->display, code, 1, 0);
    session->api->XFlush(session->display);
    session->test.FakeKeyEvent(session->display, code, 0, 0);
    session->api->XFlush(session->display);
    const std::vector<InputEvent> events = collect(*session);

    CHECK(has_key(events, Key::A, true));
    CHECK(has_key(events, Key::A, false));
    bool typed_a = false;
    for (const InputEvent& event : events) {
        if (const auto* text = std::get_if<TextInput>(&event.data); text != nullptr && text->utf8 == "a") {
            typed_a = true;
        }
    }
    CHECK(typed_a);
}

TEST_CASE("X11 input: relative motion arrives as raw MouseMoved deltas while focused", "[integration]") {
    std::string skip_reason;
    auto session = open_focused_session(skip_reason);
    if (!session) {
        SKIP(skip_reason);
    }
    centre_pointer(*session);
    session->test.FakeRelativeMotionEvent(session->display, 7, -4, 0);
    session->api->XFlush(session->display);
    const auto [dx, dy] = total_motion(collect(*session));

    // Raw motion is unaccelerated: the injected counts arrive unchanged. A layout mismatch in the raw valuators would
    // show up here as wrong or zero deltas.
    CHECK(dx == 7);
    CHECK(dy == -4);
}

TEST_CASE("X11 input: buttons and wheel steps map to the engine's mouse events", "[integration]") {
    std::string skip_reason;
    auto session = open_focused_session(skip_reason);
    if (!session) {
        SKIP(skip_reason);
    }
    // Clicks go to the window under the pointer. XWayland applies the warp asynchronously, so a click that lands elsewhere
    // is retried after warping again. The retry stops once a press arrives.
    std::vector<InputEvent> events;
    for (int attempt = 0; attempt < 3; ++attempt) {
        centre_pointer(*session);
        session->test.FakeButtonEvent(session->display, 1, 1, 0);
        session->test.FakeButtonEvent(session->display, 1, 0, 0);
        session->test.FakeButtonEvent(session->display, 4, 1, 0);  // wheel up: a press and a release, as a notch is
        session->test.FakeButtonEvent(session->display, 4, 0, 0);
        session->api->XFlush(session->display);
        events = collect(*session);
        if (std::any_of(events.begin(), events.end(), [](const InputEvent& event) {
                return std::holds_alternative<MouseButtonPressed>(event.data);
            })) {
            break;
        }
    }

    bool pressed = false;
    bool released = false;
    bool wheel_up = false;
    for (const InputEvent& event : events) {
        if (const auto* press = std::get_if<MouseButtonPressed>(&event.data); press && press->button == MouseButton::Left) {
            pressed = true;
        }
        if (const auto* release = std::get_if<MouseButtonReleased>(&event.data);
            release && release->button == MouseButton::Left) {
            released = true;
        }
        if (const auto* wheel = std::get_if<MouseWheelScrolled>(&event.data); wheel && wheel->notches_y == 1.0) {
            wheel_up = true;
        }
    }
    CHECK(pressed);
    CHECK(released);
    CHECK(wheel_up);
}

TEST_CASE("X11 input: losing keyboard focus releases the held key", "[integration]") {
    std::string skip_reason;
    auto session = open_focused_session(skip_reason);
    if (!session) {
        SKIP(skip_reason);
    }
    const auto set_focus = load_set_input_focus();
    if (!set_focus) {
        SKIP("XSetInputFocus is not available in libX11");
    }
    const x11::KeyCode code = static_cast<x11::KeyCode>(session->api->XKeysymToKeycode(session->display, 0x61));
    if (code == 0) {
        SKIP("the server keymap has no keycode for 'a'");
    }

    session->test.FakeKeyEvent(session->display, code, 1, 0);
    session->api->XFlush(session->display);
    static_cast<void>(collect(*session, 200));

    // PointerRoot focus: the window loses keyboard focus, and the key is still down on the device.
    static_cast<void>((*set_focus)(session->display, 1, 0, 0));
    session->api->XFlush(session->display);
    const std::vector<InputEvent> after = collect(*session);
    session->test.FakeKeyEvent(session->display, code, 0, 0);
    session->api->XFlush(session->display);

    if (session->platform->window_info(session->window).value().focused) {
        SKIP("the window manager moved focus back, so the focus-loss path could not run");
    }
    CHECK(has_key(after, Key::A, false));
}

TEST_CASE("X11 input: cursor capture keeps relative motion and reports the capture state", "[integration]") {
    std::string skip_reason;
    auto session = open_focused_session(skip_reason);
    if (!session) {
        SKIP(skip_reason);
    }
    REQUIRE(session->platform->set_cursor_capture(session->window, true).ok());
    CHECK(session->platform->cursor_captured(session->window).value());

    centre_pointer(*session);
    session->test.FakeRelativeMotionEvent(session->display, 3, 3, 0);
    session->api->XFlush(session->display);
    const auto [dx, dy] = total_motion(collect(*session));
    CHECK(dx == 3);
    CHECK(dy == 3);

    REQUIRE(session->platform->set_cursor_capture(session->window, false).ok());
    CHECK_FALSE(session->platform->cursor_captured(session->window).value());
}

TEST_CASE("X11 input: without /dev/input the gamepad poll reports nothing and does not fail", "[integration]") {
    std::string skip_reason;
    auto session = open_focused_session(skip_reason);
    if (!session) {
        SKIP(skip_reason);
    }
    if (access("/dev/input", F_OK) == 0) {
        SKIP("/dev/input exists on this machine, so gamepad devices are read by the evdev path instead");
    }
    for (const InputEvent& event : collect(*session, 100)) {
        CHECK(!std::holds_alternative<GamepadConnected>(event.data));
    }
}
