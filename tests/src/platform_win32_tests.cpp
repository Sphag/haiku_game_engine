// Win32 windowing against real OS windows. These tests need an interactive desktop session, which Windows CI
// runners provide. They do not claim OS focus or real monitor DPI changes; those are checked by the manual GUI run.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "engine/platform/dpi.hpp"
#include "engine/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using engine::platform::CloseRequested;
using engine::platform::FramebufferSize;
using engine::platform::Platform;
using engine::platform::PlatformErrorCode;
using engine::platform::WindowDesc;
using engine::platform::WindowDpiChanged;
using engine::platform::WindowEvent;
using engine::platform::WindowFocusChanged;
using engine::platform::WindowId;
using engine::platform::WindowInfo;
using engine::platform::WindowMinimized;
using engine::platform::WindowResized;
using engine::platform::WindowRestored;
using engine::platform::framebuffer_from_logical;

namespace {

constexpr UINT_PTR kModalTickTimerId = 1;  // matches platform_win32.cpp

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

WindowInfo info_of(const Platform& platform, WindowId id) {
    auto info = platform.window_info(id);
    REQUIRE(info.ok());
    return info.value();
}

// Returns the last event of type T for the window, if any.
template <typename T>
std::optional<T> last_event(const std::vector<WindowEvent>& events, WindowId id) {
    std::optional<T> found;
    for (const WindowEvent& event : events) {
        if (event.window == id && std::holds_alternative<T>(event.data)) {
            found = std::get<T>(event.data);
        }
    }
    return found;
}

template <typename T>
bool has_event(const std::vector<WindowEvent>& events, WindowId id) {
    return last_event<T>(events, id).has_value();
}

}  // namespace

TEST_CASE("Two windows get distinct ids, handles, and sizes", "[integration]") {
    auto platform = make_platform();

    WindowDesc first_desc;
    first_desc.title = "Окно А";
    first_desc.size = {800.0, 600.0};
    const WindowId first = platform->create_window(first_desc).value();

    WindowDesc second_desc;
    second_desc.title = "Окно Б";
    second_desc.size = {400.0, 300.0};
    const WindowId second = platform->create_window(second_desc).value();

    CHECK(first != second);
    CHECK(platform->window_count() == 2);
    CHECK(hwnd_of(*platform, first) != hwnd_of(*platform, second));
    // The handles are what a DX12 swapchain or a Vulkan Win32 surface consumes, so they must name live windows.
    CHECK(IsWindow(hwnd_of(*platform, first)) != FALSE);
    CHECK(IsWindow(hwnd_of(*platform, second)) != FALSE);
    CHECK(platform->win32_handles(first).value().hinstance != nullptr);

    // Logical size is what was requested; framebuffer size is that size times the window's DPI scale.
    const WindowInfo info = info_of(*platform, first);
    CHECK(info.logical_size.width == 800.0);
    CHECK(info.logical_size.height == 600.0);
    CHECK(info.framebuffer_size == framebuffer_from_logical(info.logical_size, info.dpi_scale));
    CHECK(info.dpi_scale >= 1.0);

    // The title is stored as UTF-16 and reads back unchanged.
    wchar_t title[64]{};
    GetWindowTextW(hwnd_of(*platform, second), title, 64);
    CHECK(std::wstring(title) == L"Окно Б");

    platform->pump_events();
    const auto events = platform->drain_events();
    CHECK(has_event<WindowResized>(events, first));
    CHECK(has_event<WindowResized>(events, second));
}

TEST_CASE("A close request leaves the window open until the application destroys it", "[integration]") {
    auto platform = make_platform();
    const WindowId first = platform->create_window(WindowDesc{}).value();
    const WindowId second = platform->create_window(WindowDesc{}).value();
    platform->pump_events();
    static_cast<void>(platform->drain_events());

    PostMessageW(hwnd_of(*platform, first), WM_CLOSE, 0, 0);
    platform->pump_events();
    const auto events = platform->drain_events();
    CHECK(has_event<CloseRequested>(events, first));
    CHECK_FALSE(has_event<CloseRequested>(events, second));

    // The request did not destroy anything.
    CHECK(platform->window_info(first).ok());

    REQUIRE(platform->destroy_window(first).ok());
    CHECK(platform->window_count() == 1);

    // Every later use of the stale id is rejected rather than reaching a freed window.
    CHECK(platform->window_info(first).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->win32_handles(first).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->destroy_window(first).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->minimize_window(first).error().code == PlatformErrorCode::NotFound);

    CHECK(platform->window_info(second).ok());
}

TEST_CASE("A resize updates the framebuffer and reports the new size", "[integration]") {
    auto platform = make_platform();
    const WindowId window = platform->create_window(WindowDesc{}).value();
    platform->pump_events();
    static_cast<void>(platform->drain_events());

    const WindowInfo before = info_of(*platform, window);
    const HWND hwnd = hwnd_of(*platform, window);
    RECT frame{};
    GetWindowRect(hwnd, &frame);
    const int width = (frame.right - frame.left) + 120;
    const int height = (frame.bottom - frame.top) + 80;
    SetWindowPos(hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    platform->pump_events();
    const auto events = platform->drain_events();
    const auto resized_event = last_event<WindowResized>(events, window);
    REQUIRE(resized_event.has_value());

    const WindowInfo after = info_of(*platform, window);
    CHECK(after.framebuffer_size.width > before.framebuffer_size.width);
    CHECK(resized_event->framebuffer_size == after.framebuffer_size);
    CHECK(resized_event->logical_size.width == after.logical_size.width);
}

TEST_CASE("Minimize and restore report an empty framebuffer and then the restored size", "[integration]") {
    auto platform = make_platform();
    const WindowId window = platform->create_window(WindowDesc{}).value();
    platform->pump_events();
    static_cast<void>(platform->drain_events());
    const WindowInfo before = info_of(*platform, window);

    REQUIRE(platform->minimize_window(window).ok());
    platform->pump_events();
    auto events = platform->drain_events();
    CHECK(has_event<WindowMinimized>(events, window));

    // A minimized window has no client area. Renderers see an empty framebuffer and must skip presenting.
    const WindowInfo minimized = info_of(*platform, window);
    CHECK(minimized.minimized);
    CHECK(minimized.framebuffer_size.empty());

    REQUIRE(platform->restore_window(window).ok());
    platform->pump_events();
    events = platform->drain_events();
    CHECK(has_event<WindowRestored>(events, window));

    const WindowInfo restored = info_of(*platform, window);
    CHECK_FALSE(restored.minimized);
    CHECK(restored.framebuffer_size == before.framebuffer_size);
}

TEST_CASE("Focus messages update focus state and emit events", "[integration]") {
    // Keyboard focus depends on the foreground window, which a test cannot control. This checks how the window
    // handles the focus messages the OS sends when it changes.
    auto platform = make_platform();
    const WindowId window = platform->create_window(WindowDesc{}).value();
    platform->pump_events();
    static_cast<void>(platform->drain_events());

    SendMessageW(hwnd_of(*platform, window), WM_SETFOCUS, 0, 0);
    platform->pump_events();
    auto events = platform->drain_events();
    REQUIRE(last_event<WindowFocusChanged>(events, window).has_value());
    CHECK(last_event<WindowFocusChanged>(events, window)->focused);
    CHECK(info_of(*platform, window).focused);

    SendMessageW(hwnd_of(*platform, window), WM_KILLFOCUS, 0, 0);
    platform->pump_events();
    events = platform->drain_events();
    REQUIRE(last_event<WindowFocusChanged>(events, window).has_value());
    CHECK_FALSE(last_event<WindowFocusChanged>(events, window)->focused);
    CHECK_FALSE(info_of(*platform, window).focused);
}

TEST_CASE("A DPI change rescales the framebuffer and keeps the logical size", "[integration]") {
    // Simulates the suggested rectangle Windows sends for a move to a 150 percent monitor.
    auto platform = make_platform();
    WindowDesc desc;
    desc.size = {640.0, 360.0};
    const WindowId window = platform->create_window(desc).value();
    platform->pump_events();
    static_cast<void>(platform->drain_events());

    const HWND hwnd = hwnd_of(*platform, window);
    constexpr UINT kTargetDpi = 144;  // 150 percent
    const FramebufferSize target = framebuffer_from_logical(desc.size, engine::platform::dpi_scale_from_dpi(kTargetDpi));

    // The frame grows with the DPI, so the outer rectangle is the target client size plus the frame at 150 percent.
    RECT frame{0, 0, static_cast<LONG>(target.width), static_cast<LONG>(target.height)};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&frame, style, FALSE, ex_style, kTargetDpi);
    RECT current{};
    GetWindowRect(hwnd, &current);
    const RECT suggested{current.left, current.top, current.left + (frame.right - frame.left),
                         current.top + (frame.bottom - frame.top)};

    SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(kTargetDpi, kTargetDpi),
                 reinterpret_cast<LPARAM>(&suggested));
    platform->pump_events();
    const auto events = platform->drain_events();

    const auto dpi_event = last_event<WindowDpiChanged>(events, window);
    REQUIRE(dpi_event.has_value());
    CHECK(dpi_event->dpi_scale == 1.5);

    const WindowInfo info = info_of(*platform, window);
    CHECK(info.dpi_scale == 1.5);
    CHECK(info.framebuffer_size == target);
    CHECK(info.logical_size.width == desc.size.width);
    CHECK(info.logical_size.height == desc.size.height);
}

TEST_CASE("Destroying a window from the modal tick is deferred until dispatch returns", "[integration]") {
    // Closing during a resize: the OS holds the thread in its size loop, and the application's tick runs inside it.
    // The tick requests destruction. The window must stay valid until the message returns, and then be gone.
    auto platform = make_platform();
    const WindowId window = platform->create_window(WindowDesc{}).value();
    const WindowId other = platform->create_window(WindowDesc{}).value();
    platform->pump_events();
    static_cast<void>(platform->drain_events());

    bool close_seen = false;
    bool tick_ran = false;
    Platform* raw = platform.get();
    platform->set_modal_tick([&]() {
        tick_ran = true;
        for (const WindowEvent& event : raw->drain_events()) {
            if (event.window == window && std::holds_alternative<CloseRequested>(event.data)) {
                close_seen = true;
                // Still valid here: destruction is deferred while the modal tick runs.
                CHECK(raw->destroy_window(window).ok());
                CHECK(raw->window_info(window).ok());
            }
        }
    });

    const HWND hwnd = hwnd_of(*platform, window);
    SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    SendMessageW(hwnd, WM_TIMER, kModalTickTimerId, 0);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);

    CHECK(tick_ran);
    CHECK(close_seen);

    platform->pump_events();
    CHECK(platform->window_info(window).error().code == PlatformErrorCode::NotFound);
    CHECK(platform->window_info(other).ok());
    CHECK(platform->window_count() == 1);
}

TEST_CASE("An exception from the modal tick is rethrown by pump_events", "[integration]") {
    auto platform = make_platform();
    const WindowId window = platform->create_window(WindowDesc{}).value();
    platform->pump_events();
    static_cast<void>(platform->drain_events());

    platform->set_modal_tick([]() { throw std::runtime_error("tick failed"); });
    const HWND hwnd = hwnd_of(*platform, window);
    SendMessageW(hwnd, WM_TIMER, kModalTickTimerId, 0);

    CHECK_THROWS_AS(platform->pump_events(), std::runtime_error);

    // The error is delivered once; the platform remains usable.
    platform->set_modal_tick({});
    CHECK_NOTHROW(platform->pump_events());
    CHECK(platform->window_info(window).ok());
}

TEST_CASE("A platform can be created again after the previous one is destroyed", "[integration]") {
    {
        auto first = make_platform();
        CHECK(first->create_window(WindowDesc{}).ok());
    }
    auto second = make_platform();
    const WindowId window = second->create_window(WindowDesc{}).value();
    CHECK(second->window_info(window).ok());
}

// GetCursorPos and ScreenToClient give the pointer in client pixels; a destroyed window's id no longer resolves.
TEST_CASE("The pointer position is in client pixels, and a destroyed window id reports NotFound", "[integration]") {
    auto platform = make_platform();
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
