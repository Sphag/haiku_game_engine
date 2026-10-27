#pragma once

#include "engine/rhi/error.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <cstdint>
#include <optional>

namespace engine::rhi {

// Presentation policy. With vsync, each present waits for the display's vertical blank. Without vsync, the swapchain
// presents immediately with tearing when allow_tearing is set and the adapter reports support; otherwise vsync stays on,
// and present_settings() reports that the request was not honored.
struct PresentSettings {
    bool vsync = true;
    bool allow_tearing = true;
};

// The native window a swapchain presents to. Each backend reads the fields it needs and ignores the others: DX12 uses hwnd;
// Vulkan on Windows uses hinstance and hwnd; Vulkan on X11 uses x11_display and x11_window. The window must outlive the swapchain.
struct SwapchainDesc {
    void* hwnd = nullptr;          // HWND
    void* hinstance = nullptr;     // HINSTANCE (Vulkan on Windows)
    void* x11_display = nullptr;   // Display* (Vulkan on X11)
    std::uint64_t x11_window = 0;  // Window XID (Vulkan on X11)
    std::uint32_t width = 0;   // framebuffer pixels (Platform::window_info); both must be nonzero
    std::uint32_t height = 0;
    Format format = Format::B8G8R8A8Unorm;
    std::uint32_t buffer_count = 2;  // two back buffers allow two frames in flight
    PresentSettings present;
};

// Owns the back buffers of one window. A frame is: acquire_back_buffer(), record work that leaves the buffer in Present, submit
// it on the graphics queue, then present(). Presenting signals a frame fence for that buffer, and acquiring the buffer again waits
// for that fence, so the CPU never overwrites a buffer the GPU is still presenting.
//
// Lifetime: destroy every swapchain before the device that created it. Its back buffers are device objects.
class Swapchain {
public:
    virtual ~Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Returns the back buffer for this frame. Returns nothing while the surface is empty (minimized); skip the frame then.
    virtual RhiResult<std::optional<TextureHandle>> acquire_back_buffer() = 0;
    // Presents the buffer returned by the last acquire. The buffer must be in the Present state.
    virtual RhiStatus present() = 0;
    // Changes the surface size after the GPU is idle. A zero dimension empties the surface (minimized): acquire returns
    // nothing until a nonzero size arrives. A nonzero size recreates the back buffers.
    // A surface with a fixed extent (Vulkan on Windows and X11 reports one) is built at that extent, so the size in use can
    // differ from the one requested. width() and height() report the size in use.
    virtual RhiStatus resize(std::uint32_t width, std::uint32_t height) = 0;

    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    // The policy in effect, after the adapter's support is taken into account.
    [[nodiscard]] virtual PresentSettings present_settings() const noexcept = 0;

protected:
    Swapchain() = default;
};

}  // namespace engine::rhi
