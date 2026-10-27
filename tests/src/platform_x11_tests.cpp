// X11 windowing against a real X server (XWayland or Xorg). These tests need DISPLAY to point at a running server.
// Without one they report SKIP, which Catch2 counts as not-run rather than passed. Window-manager behaviour (iconify
// and focus) also depends on the window manager; a test that the window manager does not honour reports SKIP too.
// Resize and close are sent by this test, so they do not depend on a window manager.
#include "engine/platform/dpi.hpp"
#include "engine/platform/platform.hpp"

#include "xlib_abi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using engine::platform::CloseRequested;
using engine::platform::FramebufferSize;
using engine::platform::Platform;
using engine::platform::PlatformErrorCode;
using engine::platform::WindowDesc;
using engine::platform::WindowEvent;
using engine::platform::WindowFocusChanged;
using engine::platform::WindowId;
using engine::platform::WindowInfo;
using engine::platform::WindowMinimized;
using engine::platform::WindowResized;
using engine::platform::WindowRestored;
using engine::platform::framebuffer_from_logical;
using engine::platform::x11::Api;
namespace x11 = engine::platform::x11;

namespace {

// Mirrors XWindowAttributes from <X11/Xutil.h> (x86-64 Linux). Xlib writes the whole struct, so every field is here.
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

// Functions the backend does not need but the tests use to drive windows from outside the backend.
struct DriverApi {
    x11::Status (*XSendEvent)(x11::Display* display, x11::Window window, x11::Bool propagate, long event_mask,
                              x11::XEvent* event);
    int (*XResizeWindow)(x11::Display* display, x11::Window window, unsigned int width, unsigned int height);
    int (*XSetInputFocus)(x11::Display* display, x11::Window focus, int revert_to, unsigned long time);
    int (*XGetWindowAttributes)(x11::Display* display, x11::Window window, XWindowAttributesLayout* attributes);
    int (*XUnmapWindow)(x11::Display* display, x11::Window window);
};

constexpr int kIsViewable = 2;  // map_state of a mapped window that the server shows

template <typename Fn>
void bind_symbol(void* library, const char* name, Fn& out) {
    void* symbol = dlsym(library, name);
    REQUIRE(symbol != nullptr);
    std::memcpy(&out, &symbol, sizeof(out));
}

const DriverApi& driver() {
    static DriverApi api{};
    static bool bound = false;
    if (!bound) {
        void* library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        REQUIRE(library != nullptr);
        bind_symbol(library, "XSendEvent", api.XSendEvent);
        bind_symbol(library, "XResizeWindow", api.XResizeWindow);
        bind_symbol(library, "XSetInputFocus", api.XSetInputFocus);
        bind_symbol(library, "XGetWindowAttributes", api.XGetWindowAttributes);
        bind_symbol(library, "XUnmapWindow", api.XUnmapWindow);
        bound = true;
    }
    return api;
}

// Opens the platform, or explains why this run cannot exercise X11 windows.
std::unique_ptr<Platform> open_platform(std::string& skip_reason) {
    auto created = Platform::create();
    if (created.ok()) {
        return std::move(created).value();
    }
    skip_reason = std::string(engine::platform::to_string(created.error().code)) + ": " + created.error().detail;
    return nullptr;
}

// Pumps events until `done` returns true for the accumulated log or the timeout passes.
template <typename Done>
bool wait_for(Platform& platform, std::vector<WindowEvent>& log, Done done) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(3);
    while (true) {
        platform.pump_events();
        for (WindowEvent& event : platform.drain_events()) {
            log.push_back(std::move(event));
        }
        if (done(log)) {
            return true;
        }
        if (clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Pumps for a fixed time, to show that an event did not arrive.
void pump_for(Platform& platform, std::vector<WindowEvent>& log, std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        platform.pump_events();
        for (WindowEvent& event : platform.drain_events()) {
            log.push_back(std::move(event));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

template <typename T>
std::optional<T> last_event(const std::vector<WindowEvent>& log, WindowId id) {
    std::optional<T> found;
    for (const WindowEvent& event : log) {
        if (event.window == id && std::holds_alternative<T>(event.data)) {
            found = std::get<T>(event.data);
        }
    }
    return found;
}

// True when the log holds a focus event with this value. The window manager may change focus again afterwards, so
// the last event is not used.
bool saw_focus(const std::vector<WindowEvent>& log, WindowId id, bool focused) {
    for (const WindowEvent& event : log) {
        const auto* change = std::get_if<WindowFocusChanged>(&event.data);
        if (event.window == id && change != nullptr && change->focused == focused) {
            return true;
        }
    }
    return false;
}

// True when the log holds a resize to this framebuffer size.
bool has_resized_to(const std::vector<WindowEvent>& log, WindowId id, FramebufferSize size) {
    for (const WindowEvent& event : log) {
        const auto* resized = std::get_if<WindowResized>(&event.data);
        if (event.window == id && resized != nullptr && resized->framebuffer_size == size) {
            return true;
        }
    }
    return false;
}

template <typename T>
bool has_event(const std::vector<WindowEvent>& log, WindowId id) {
    return last_event<T>(log, id).has_value();
}

WindowInfo info_of(const Platform& platform, WindowId id) {
    auto info = platform.window_info(id);
    REQUIRE(info.ok());
    return info.value();
}

x11::Window xid_of(const Platform& platform, WindowId id) {
    auto handles = platform.xlib_handles(id);
    REQUIRE(handles.ok());
    return static_cast<x11::Window>(handles.value().window);
}

x11::Display* display_of(const Platform& platform, WindowId id) {
    auto handles = platform.xlib_handles(id);
    REQUIRE(handles.ok());
    return static_cast<x11::Display*>(handles.value().display);
}

// Waits until the server shows the window and returns the last map_state it reported (kIsViewable on success). The
// window manager maps new windows asynchronously, and focus requests made earlier fail with BadMatch.
int wait_viewable(Platform& platform, WindowId id) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(10);
    x11::Display* display = display_of(platform, id);
    const x11::Window window = xid_of(platform, id);
    int state = -1;
    while (true) {
        x11::load_api(nullptr)->XFlush(display);
        XWindowAttributesLayout attributes{};
        if (driver().XGetWindowAttributes(display, window, &attributes) != 0) {
            state = attributes.map_state;
        }
        if (state == kIsViewable || clock::now() >= deadline) {
            return state;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Size the server reports for the window, read independently of the backend's bookkeeping.
FramebufferSize server_size(const Platform& platform, WindowId id) {
    const Api& api = *x11::load_api(nullptr);
    x11::Window root = 0;
    int x = 0;
    int y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int border = 0;
    unsigned int depth = 0;
    api.XGetGeometry(display_of(platform, id), xid_of(platform, id), &root, &x, &y, &width, &height, &border, &depth);
    return {width, height};
}

}  // namespace

TEST_CASE("A missing display is reported as NoDisplay", "[integration]") {
    // The CPU-only parts of the engine do not depend on a window, so a failed open is a value, not a crash.
    const char* previous = std::getenv("DISPLAY");
    const std::string saved = previous != nullptr ? previous : "";
    // "unix:" limits Xlib to the Unix socket. Without it, Xlib falls back to TCP on localhost, which can take minutes
    // to time out. Display 4321 has no socket, so the open fails at once.
    REQUIRE(setenv("DISPLAY", "unix:4321", 1) == 0);

    auto created = Platform::create();

    if (previous != nullptr) {
        setenv("DISPLAY", saved.c_str(), 1);
    } else {
        unsetenv("DISPLAY");
    }
    REQUIRE_FALSE(created.ok());
    CHECK(created.error().code == PlatformErrorCode::NoDisplay);
    CHECK(created.error().detail.find("unix:4321") != std::string::npos);
}

TEST_CASE("Two windows get distinct X ids, live server windows, and logical sizes", "[integration]") {
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }

    WindowDesc first_desc;
    first_desc.title = "Окно А";
    first_desc.size = {800.0, 600.0};
    const WindowId first = platform->create_window(first_desc).value();

    WindowDesc second_desc;
    second_desc.title = "Окно Б";
    second_desc.size = {400.0, 300.0};
    const WindowId second = platform->create_window(second_desc).value();

    CHECK_FALSE(first == second);
    CHECK(platform->window_count() == 2);
    CHECK(xid_of(*platform, first) != xid_of(*platform, second));
    CHECK(display_of(*platform, first) != nullptr);
    CHECK(display_of(*platform, first) == display_of(*platform, second));

    // Both windows report their first size before the sizes are compared. The window manager may still adjust a
    // new window, and the backend's bookkeeping only changes when events are pumped.
    std::vector<WindowEvent> log;
    CHECK(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowResized>(seen, first) && has_event<WindowResized>(seen, second);
    }));

    // The server knows both windows, and its size matches the framebuffer the backend reports.
    const WindowInfo info = info_of(*platform, first);
    CHECK(server_size(*platform, first) == info.framebuffer_size);
    CHECK(server_size(*platform, second) == info_of(*platform, second).framebuffer_size);

    CHECK(info.logical_size.width == 800.0);
    CHECK(info.logical_size.height == 600.0);
    CHECK(info.framebuffer_size == framebuffer_from_logical(info.logical_size, info.dpi_scale));
    CHECK(info.dpi_scale >= 1.0);
}

TEST_CASE("A close request leaves the window open until the application destroys it", "[integration]") {
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }
    const WindowId first = platform->create_window(WindowDesc{}).value();
    const WindowId second = platform->create_window(WindowDesc{}).value();
    std::vector<WindowEvent> log;
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowResized>(seen, first) && has_event<WindowResized>(seen, second);
    }));
    log.clear();

    // WM_DELETE_WINDOW is what a window manager sends when its close button is pressed.
    x11::Display* display = display_of(*platform, first);
    const x11::Api& api = *x11::load_api(nullptr);
    x11::XEvent message{};
    message.xclient.type = x11::kClientMessage;
    message.xclient.display = display;
    message.xclient.window = xid_of(*platform, first);
    message.xclient.message_type = api.XInternAtom(display, "WM_PROTOCOLS", 0);
    message.xclient.format = 32;
    message.xclient.data.l[0] = static_cast<long>(api.XInternAtom(display, "WM_DELETE_WINDOW", 0));
    driver().XSendEvent(display, xid_of(*platform, first), 0, 0, &message);
    api.XFlush(display);

    CHECK(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<CloseRequested>(seen, first);
    }));
    CHECK_FALSE(has_event<CloseRequested>(log, second));

    // The request did not destroy anything.
    CHECK(platform->window_info(first).ok());

    REQUIRE(platform->destroy_window(first).ok());
    CHECK(platform->window_count() == 1);

    // Every later use of the stale id is rejected rather than reaching another window.
    CHECK(platform->window_info(first).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->xlib_handles(first).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->destroy_window(first).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->minimize_window(first).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->window_info(second).ok());
}

TEST_CASE("A resize updates the framebuffer and reports the new size", "[integration]") {
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }
    const WindowId window = platform->create_window(WindowDesc{}).value();
    std::vector<WindowEvent> log;
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowResized>(seen, window);
    }));
    // A reparenting window manager must finish managing the window before it reliably answers a resize.
    const int state = wait_viewable(*platform, window);
    if (state != kIsViewable) {
        SKIP("the window manager did not show the window (map_state " + std::to_string(state) + ")");
    }
    log.clear();
    const WindowInfo before = info_of(*platform, window);

    // XResizeWindow takes device pixels, which is the framebuffer size.
    driver().XResizeWindow(display_of(*platform, window), xid_of(*platform, window), 900, 500);
    x11::load_api(nullptr)->XFlush(display_of(*platform, window));

    const FramebufferSize expected{900, 500};
    const bool grown = wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        const auto resized = last_event<WindowResized>(seen, window);
        return resized.has_value() && resized->framebuffer_size == expected;
    });
    const FramebufferSize reported = server_size(*platform, window);
    if (!grown && !(reported == expected)) {
        // The window manager may keep or change the size it chose, so a server that did not take the request is a
        // window-manager refusal. A server at the requested size without a backend event is a backend failure.
        SKIP("the window manager did not apply the resize (server size " + std::to_string(reported.width) + "x" +
             std::to_string(reported.height) + ")");
    }
    CHECK(grown);

    const WindowInfo after = info_of(*platform, window);
    CHECK_FALSE(before.framebuffer_size == expected);
    CHECK(after.framebuffer_size == expected);
    CHECK(after.framebuffer_size == server_size(*platform, window));
    const auto resized_event = last_event<WindowResized>(log, window);
    REQUIRE(resized_event.has_value());
    CHECK(resized_event->framebuffer_size == after.framebuffer_size);
    CHECK(resized_event->logical_size.width == after.logical_size.width);
}

TEST_CASE("Minimize and restore report an empty framebuffer and then the restored size", "[integration]") {
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }
    const WindowId window = platform->create_window(WindowDesc{}).value();
    std::vector<WindowEvent> log;
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowResized>(seen, window);
    }));
    log.clear();
    const WindowInfo before = info_of(*platform, window);

    const int state = wait_viewable(*platform, window);
    if (state != kIsViewable) {
        SKIP("the window manager did not show the window (map_state " + std::to_string(state) + ")");
    }
    REQUIRE(platform->minimize_window(window).ok());
    // Check the live state in the same predicate as the event: the window manager may restore the window right after.
    if (!wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
            return has_event<WindowMinimized>(seen, window) && info_of(*platform, window).minimized;
        })) {
        SKIP("the window manager did not minimize the window; minimize needs a compliant window manager");
    }

    // A minimized window has no client area. Renderers see an empty framebuffer and must skip presenting.
    const WindowInfo minimized = info_of(*platform, window);
    CHECK(minimized.minimized);
    CHECK(minimized.framebuffer_size.empty());

    REQUIRE(platform->restore_window(window).ok());
    CHECK(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowRestored>(seen, window);
    }));

    const WindowInfo restored = info_of(*platform, window);
    CHECK_FALSE(restored.minimized);
    CHECK(restored.framebuffer_size == before.framebuffer_size);
}

TEST_CASE("Keyboard focus moves between windows and emits focus events", "[integration]") {
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }
    const WindowId first = platform->create_window(WindowDesc{}).value();
    const WindowId second = platform->create_window(WindowDesc{}).value();
    std::vector<WindowEvent> log;
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowResized>(seen, first) && has_event<WindowResized>(seen, second);
    }));
    log.clear();

    const int first_state = wait_viewable(*platform, first);
    if (first_state != kIsViewable) {
        SKIP("the window manager did not show the window (map_state " + std::to_string(first_state) + ")");
    }
    driver().XSetInputFocus(display_of(*platform, first), xid_of(*platform, first), x11::kRevertToParent,
                            x11::kCurrentTime);
    x11::load_api(nullptr)->XFlush(display_of(*platform, first));
    if (!wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
            const auto focus = last_event<WindowFocusChanged>(seen, first);
            return focus.has_value() && focus->focused;
        })) {
        SKIP("no focus change arrived; the window manager did not grant focus to the test window");
    }
    CHECK(info_of(*platform, first).focused);

    REQUIRE(wait_viewable(*platform, second) == kIsViewable);
    driver().XSetInputFocus(display_of(*platform, second), xid_of(*platform, second), x11::kRevertToParent,
                            x11::kCurrentTime);
    x11::load_api(nullptr)->XFlush(display_of(*platform, second));
    CHECK(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        const auto focus = last_event<WindowFocusChanged>(seen, first);
        return focus.has_value() && !focus->focused;
    }));
    CHECK_FALSE(info_of(*platform, first).focused);
}

TEST_CASE("Unmapping and mapping a window report minimize and restore without a window manager", "[integration]") {
    // Synthetic check of the backend's handling: the application never unmaps its own windows, so an unmap is the
    // minimize signal. The assertions use the events that arrive, not the window's state afterwards, because a
    // window manager may map the window again right away.
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }
    const WindowId window = platform->create_window(WindowDesc{}).value();
    std::vector<WindowEvent> log;
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowResized>(seen, window);
    }));
    // Unmapping a window whose frame the window manager has not mapped produces no event, so wait for the window to show.
    const int state = wait_viewable(*platform, window);
    if (state != kIsViewable) {
        SKIP("the window manager did not show the window (map_state " + std::to_string(state) + ")");
    }
    log.clear();
    const FramebufferSize before = info_of(*platform, window).framebuffer_size;

    x11::Display* display = display_of(*platform, window);
    driver().XUnmapWindow(display, xid_of(*platform, window));
    x11::load_api(nullptr)->XFlush(display);
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowMinimized>(seen, window);
    }));
    CHECK(has_resized_to(log, window, FramebufferSize{}));

    x11::load_api(nullptr)->XMapRaised(display, xid_of(*platform, window));
    x11::load_api(nullptr)->XFlush(display);
    const auto restored = [&](const std::vector<WindowEvent>& seen) { return has_event<WindowRestored>(seen, window); };
    if (!wait_for(*platform, log, restored)) {
        // The window manager decides whether a remapped window is shown again. A window the server does not show
        // is a window-manager refusal (skip); a shown window without MapNotify is a backend failure (checked below).
        if (wait_viewable(*platform, window) != kIsViewable) {
            SKIP("the window manager did not map the window back");
        }
        pump_for(*platform, log, std::chrono::milliseconds(100));
    }
    REQUIRE(restored(log));
    CHECK(has_resized_to(log, window, before));
}

TEST_CASE("Focus events from the server update focus state, and grabs are ignored", "[integration]") {
    // Synthetic check of the backend's handling: focus events are sent to the window by this test. The focus
    // requests themselves are not performed, so this does not depend on the window manager granting focus.
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }
    const WindowId window = platform->create_window(WindowDesc{}).value();
    std::vector<WindowEvent> log;
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return has_event<WindowResized>(seen, window);
    }));
    log.clear();

    x11::Display* display = display_of(*platform, window);
    const x11::Window xid = xid_of(*platform, window);
    const auto send_focus = [&](int type, int mode) {
        x11::XEvent event{};
        event.xfocus.type = type;
        event.xfocus.display = display;
        event.xfocus.window = xid;
        event.xfocus.mode = mode;
        event.xfocus.detail = 3;  // NotifyNonlinear
        driver().XSendEvent(display, xid, 0, x11::kFocusChangeMask, &event);
        x11::load_api(nullptr)->XFlush(display);
    };

    // Establish a known state first: the window manager may have given the window focus already.
    send_focus(x11::kFocusOut, x11::kNotifyNormal);
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return saw_focus(seen, window, false);
    }));
    // The window manager also sends focus events while it settles the new windows. Let that finish before the
    // grab check, so the check sees only the synthetic events.
    pump_for(*platform, log, std::chrono::milliseconds(500));
    log.clear();
    const bool focused_before = info_of(*platform, window).focused;

    // A focus change caused by a pointer grab (NotifyGrab = 1) must not change keyboard focus or emit an event.
    send_focus(x11::kFocusIn, 1);
    send_focus(x11::kFocusOut, 1);
    pump_for(*platform, log, std::chrono::milliseconds(300));
    CHECK_FALSE(has_event<WindowFocusChanged>(log, window));
    CHECK(info_of(*platform, window).focused == focused_before);

    send_focus(x11::kFocusIn, x11::kNotifyNormal);
    REQUIRE(wait_for(*platform, log, [&](const std::vector<WindowEvent>& seen) {
        return saw_focus(seen, window, true);
    }));
    CHECK(info_of(*platform, window).focused);
}

TEST_CASE("A platform can be created again after the previous one is destroyed", "[integration]") {
    std::string reason;
    auto first = open_platform(reason);
    if (!first) {
        SKIP("X display not available (" + reason + ")");
    }
    CHECK(first->create_window(WindowDesc{}).ok());
    first.reset();

    auto second = open_platform(reason);
    REQUIRE(second != nullptr);
    const WindowId window = second->create_window(WindowDesc{}).value();
    CHECK(second->window_info(window).ok());
}

// The pointer position is read from the server, so it is absolute; a destroyed window's id no longer resolves.
TEST_CASE("The pointer position is absolute, and a destroyed window id reports NotFound", "[integration]") {
    std::string reason;
    auto platform = open_platform(reason);
    if (!platform) {
        SKIP("X display not available (" + reason + ")");
    }
    const WindowId window = platform->create_window(WindowDesc{}).value();
    const auto position = platform->pointer_position(window);
    REQUIRE(position.ok());
    CHECK(std::isfinite(position.value().x));
    CHECK(std::isfinite(position.value().y));

    REQUIRE(platform->destroy_window(window).ok());
    const auto stale = platform->pointer_position(window);
    REQUIRE_FALSE(stale.ok());
    CHECK(stale.error().code == PlatformErrorCode::NotFound);
}
