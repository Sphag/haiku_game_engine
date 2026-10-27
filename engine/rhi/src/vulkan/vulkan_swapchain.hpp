#pragma once

#include "vulkan/vulkan_device.hpp"

#include "engine/rhi/swapchain.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace engine::rhi::vulkan {

// The native window a surface is created from. Windows fills hinstance and hwnd; X11 fills display and window.
struct NativeWindow {
    void* hinstance = nullptr;
    void* hwnd = nullptr;
    void* display = nullptr;
    std::uint64_t window = 0;
};

// A swapchain on the device's graphics queue, with its surface. Each acquired image is ordered by two binary semaphores: the
// acquire semaphore is waited on by the frame's first graphics submission, and that submission signals the image's render-finished
// semaphore, which present waits on. Frames in flight are bounded by the image count, so no image is handed out while a frame that
// used it is still presenting.
//
// Out-of-date and suboptimal results rebuild the swapchain at the last requested size before the next acquire. A lost surface is
// dropped with its images and rebuilt from the saved native window by the next acquire or resize; the frame that found the loss is
// skipped.
class VulkanSwapchain final : public Swapchain {
public:
    // Fails with Unsupported when the device lacks the surface extensions, the graphics queue cannot present to the surface, or the
    // surface has no sRGB format for the requested one.
    [[nodiscard]] static RhiResult<std::unique_ptr<Swapchain>> create(VulkanDevice& device, const SwapchainDesc& desc);
    ~VulkanSwapchain() override;

    RhiResult<std::optional<TextureHandle>> acquire_back_buffer() override;
    RhiStatus present() override;
    RhiStatus resize(std::uint32_t width, std::uint32_t height) override;

    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] PresentSettings present_settings() const noexcept override;

    // Device hooks. add_frame_sync adds this swapchain's pending wait and signal to a graphics submission and returns true when one is
    // owed; frame_sync_submitted records that the submission was accepted.
    [[nodiscard]] bool add_frame_sync(SubmitSync& sync) const;
    void frame_sync_submitted();

private:
    VulkanSwapchain(VulkanDevice& device, NativeWindow window, Format format, std::uint32_t buffer_count, PresentSettings requested,
                    std::uint32_t width, std::uint32_t height);

    RhiStatus initialize();
    RhiStatus create_surface();
    RhiStatus build(VkSwapchainKHR previous);
    RhiStatus recreate(std::uint32_t width, std::uint32_t height);
    RhiStatus rebuild_surface();
    void drop_surface();
    RhiStatus create_sync_objects();
    void release_images();
    void destroy_sync_objects();

    VulkanDevice& device_;
    NativeWindow native_;
    Format format_;
    VkFormat vk_format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t requested_buffers_;
    PresentSettings requested_present_;
    PresentSettings settings_;  // effective: vsync is forced on when tearing is unavailable
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::uint32_t width_;   // 0 while the surface is empty
    std::uint32_t height_;
    std::vector<TextureHandle> buffers_;
    std::vector<VkSemaphore> acquire_semaphores_;  // one more than the image count, rotated so a reused one was waited on
    std::vector<VkSemaphore> render_done_;         // one per image
    std::uint32_t acquire_slot_ = 0;
    std::uint32_t current_ = 0;
    VkSemaphore pending_acquire_ = VK_NULL_HANDLE;  // the semaphore the last acquire signaled
    bool acquired_ = false;
    bool acquire_pending_ = false;  // acquired, and no graphics submission has waited on the acquire yet
    bool needs_rebuild_ = false;
    bool surface_lost_ = false;
};

}  // namespace engine::rhi::vulkan
