#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace engine::platform {

// Identifies a window for as long as it exists. The generation changes when a slot is reused, so an id kept after
// its window was destroyed is rejected (NotFound) instead of reaching another window.
struct WindowId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;  // 0 is never a live generation

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation != 0;
    }

    friend constexpr bool operator==(const WindowId& lhs, const WindowId& rhs) noexcept {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }
};

// Size in physical pixels of the client area. This is what swapchains and render targets use.
// It is 0x0 while a window is minimized.
struct FramebufferSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return width == 0 || height == 0;
    }

    friend constexpr bool operator==(const FramebufferSize& lhs, const FramebufferSize& rhs) noexcept {
        return lhs.width == rhs.width && lhs.height == rhs.height;
    }
};

// Size in device-independent pixels: framebuffer size divided by the window's DPI scale. Layout and UI use this.
struct LogicalSize {
    double width = 0.0;
    double height = 0.0;
};

// Requested window properties at creation. The size is logical; the framebuffer size follows from the DPI scale.
struct WindowDesc {
    std::string title = "Engine";  // UTF-8
    LogicalSize size{1280.0, 720.0};
    bool resizable = true;
};

// Native handles for graphics APIs. Pointers are returned as void* so this header stays free of OS headers.
// DX12 swapchains use hwnd; Vulkan's VkWin32SurfaceCreateInfoKHR uses hinstance and hwnd.
struct Win32WindowHandles {
    void* hinstance = nullptr;  // HINSTANCE
    void* hwnd = nullptr;       // HWND
};

// Native handles for Xlib (X11 backend). VkXlibSurfaceCreateInfoKHR takes the Display and the Window. The screen is
// kept for window-manager requests. Pointers and XIDs are plain integers so this header stays free of X11 headers.
struct XlibWindowHandles {
    void* display = nullptr;   // Display*; owned by the Platform, valid until it is destroyed
    std::uint64_t window = 0;  // Window (XID) of the live window
    int screen = 0;
};

struct WindowInfo {
    WindowId id;
    LogicalSize logical_size;
    FramebufferSize framebuffer_size;
    double dpi_scale = 1.0;  // 1.0 is 96 DPI; the framebuffer is the logical size times this value
    bool focused = false;
    bool minimized = false;
};

// Events are queued and read with Platform::drain_events(). Handlers never run application code.
struct CloseRequested {};  // the window is still open; the application decides whether to destroy it
struct WindowResized {
    LogicalSize logical_size;
    FramebufferSize framebuffer_size;
};
struct WindowMinimized {};
struct WindowRestored {};
struct WindowFocusChanged {
    bool focused = false;
};
struct WindowDpiChanged {
    double dpi_scale = 1.0;
};

using WindowEventData = std::variant<CloseRequested, WindowResized, WindowMinimized, WindowRestored,
                                     WindowFocusChanged, WindowDpiChanged>;

struct WindowEvent {
    WindowId window;
    WindowEventData data;
};

enum class PlatformErrorCode {
    Unsupported,       // no windowing backend exists for this operating system yet
    InvalidArgument,   // rejected WindowDesc (calls from a thread other than the owner assert instead)
    CreateFailed,      // the OS refused to create a window or register its class
    NotFound,          // the id is stale or was never issued
    NoDisplay,         // no X display or X11 client library (Linux): CPU-only work still runs, windows do not
};

struct PlatformError {
    PlatformErrorCode code = PlatformErrorCode::Unsupported;
    std::string detail;
};

[[nodiscard]] std::string_view to_string(PlatformErrorCode code) noexcept;

// Checks title (valid UTF-8, no embedded NUL) and size (finite, at least 1 DIP, at most kMaxLogicalDimension).
[[nodiscard]] core::Result<core::Unit, PlatformError> validate_window_desc(const WindowDesc& desc);

inline constexpr double kMaxLogicalDimension = 16384.0;

}  // namespace engine::platform
