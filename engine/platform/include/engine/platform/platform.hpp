#pragma once

#include "engine/core/result.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/window.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace engine::platform {

struct PointerPosition {
    double x = 0.0;  // framebuffer pixels from the client area's left edge
    double y = 0.0;  // framebuffer pixels from the client area's top edge
};

// Owns the native windows of one process. Every call must come from the thread that called create(); window
// messages are only delivered to that thread. Windows are created and destroyed through the returned WindowIds.
//
// Closing: a user's close button produces CloseRequested and leaves the window open. The application calls
// destroy_window() when it is ready.
//
// Resizing: while the OS holds the thread in its move/size loop, pump_events() does not return. To keep rendering
// during the drag, the modal tick (set_modal_tick) runs about every 16 ms from inside that loop. The tick may call
// create_window(), destroy_window(), and drain_events(), but it must not call pump_events().
//
// Zero size: a minimized window has an empty framebuffer. Renderers must skip presenting while it is empty and
// must not create swapchains with zero dimensions.
class Platform {
public:
    using ModalTick = std::function<void()>;

    // Selects per-monitor DPI awareness for the process and registers the window class (Win32). On Linux, opens the X
    // display (X11). Returns NoDisplay when no display or X11 library is available, and Unsupported on an operating
    // system without an in-house backend.
    [[nodiscard]] static core::Result<std::unique_ptr<Platform>, PlatformError> create();

    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    [[nodiscard]] core::Result<WindowId, PlatformError> create_window(const WindowDesc& desc);

    // Destroys the window at once, or after the current message returns if called from inside one.
    [[nodiscard]] core::Result<core::Unit, PlatformError> destroy_window(WindowId id);

    [[nodiscard]] core::Result<core::Unit, PlatformError> minimize_window(WindowId id);
    [[nodiscard]] core::Result<core::Unit, PlatformError> restore_window(WindowId id);

    [[nodiscard]] core::Result<WindowInfo, PlatformError> window_info(WindowId id) const;

    // Native handles for the graphics backend. Valid until the window is destroyed.
    [[nodiscard]] core::Result<Win32WindowHandles, PlatformError> win32_handles(WindowId id) const;

    // Display and window for Xlib. Valid until the window is destroyed. Only the X11 backend returns them; the Win32
    // backend returns Unsupported.
    [[nodiscard]] core::Result<XlibWindowHandles, PlatformError> xlib_handles(WindowId id) const;

    // Dispatches every pending window message, then destroys windows whose destruction was deferred.
    void pump_events();

    // Returns the queued events in order and empties the queue.
    [[nodiscard]] std::vector<WindowEvent> drain_events();

    // Input events since the last call, in order. Keyboard, text, and mouse events come from the window that has focus.
    // Gamepad events have no window. Redundant transitions are never queued (see InputState). Linux reads keys and text
    // from X11, relative motion from XInput2, and gamepads from evdev (see docs/input.md).
    [[nodiscard]] std::vector<InputEvent> drain_input_events();

    // Captures the cursor for a window. While the window is focused the cursor is hidden and confined to the client
    // area. Relative motion arrives as MouseMoved either way. Capture is released on focus loss and on destruction, and
    // it is applied again when focus returns. On X11 the pointer is grabbed and confined while captured.
    [[nodiscard]] core::Result<core::Unit, PlatformError> set_cursor_capture(WindowId id, bool captured);
    [[nodiscard]] core::Result<bool, PlatformError> cursor_captured(WindowId id) const;

    // The pointer's position now, in framebuffer pixels from the client area's top-left corner. It can lie outside the window.
    // Unlike MouseMoved, which is relative, this is absolute, so an editor can hit-test the pointer while it is not captured.
    // Fails with NotFound for a stale id. The position is read from the OS, so a window without focus still reports where the
    // pointer is; the caller decides whether that counts as hovering the window.
    [[nodiscard]] core::Result<PointerPosition, PlatformError> pointer_position(WindowId id) const;

    // An empty function removes the tick.
    void set_modal_tick(ModalTick tick);

    [[nodiscard]] std::size_t window_count() const noexcept;

private:
    struct Impl;
    explicit Platform(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace engine::platform
