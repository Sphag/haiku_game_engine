#include "vulkan/vulkan_swapchain.hpp"

#include "vulkan/vulkan_common.hpp"

#include "engine/core/result.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace engine::rhi::vulkan {
namespace {

constexpr std::uint32_t kMinBufferCount = 2;
constexpr std::uint32_t kMaxBufferCount = 16;

#ifdef _WIN32
constexpr const char* kPlatformSurfaceExtension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
constexpr const char* kPlatformSurfaceExtension = "VK_KHR_xlib_surface";
#endif

VkCompositeAlphaFlagBitsKHR pick_composite_alpha(VkCompositeAlphaFlagsKHR supported) {
    if ((supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    for (VkCompositeAlphaFlagBitsKHR bit : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if ((supported & bit) != 0) {
            return bit;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

std::uint32_t clamp_extent(std::uint32_t value, std::uint32_t minimum, std::uint32_t maximum) {
    return std::clamp(value, minimum, maximum);
}

}  // namespace

VulkanSwapchain::VulkanSwapchain(VulkanDevice& device, NativeWindow window, Format format, std::uint32_t buffer_count,
                                 PresentSettings requested, std::uint32_t width, std::uint32_t height)
    : device_(device),
      native_(window),
      format_(format),
      requested_buffers_(buffer_count),
      requested_present_(requested),
      settings_(requested),
      width_(width),
      height_(height) {}

VulkanSwapchain::~VulkanSwapchain() {
    device_.unregister_swapchain(this);
    // The images and their views may still be presented or read by queued work. Wait, then release them before the surface goes.
    static_cast<void>(device_.wait_idle());
    release_images();
    destroy_sync_objects();
    const Api& api = device_.api();
    if (swapchain_ != VK_NULL_HANDLE) {
        api.vkDestroySwapchainKHR(device_.handle(), swapchain_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        api.vkDestroySurfaceKHR(device_.instance(), surface_, nullptr);
    }
}

RhiResult<std::unique_ptr<Swapchain>> VulkanSwapchain::create(VulkanDevice& device, const SwapchainDesc& desc) {
#ifdef _WIN32
    if (desc.hwnd == nullptr || desc.hinstance == nullptr) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::InvalidDescriptor, "create_swapchain: hwnd and hinstance are required on Windows");
    }
#else
    if (desc.x11_display == nullptr || desc.x11_window == 0) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::InvalidDescriptor, "create_swapchain: x11_display and x11_window are required on X11");
    }
#endif
    if (desc.width == 0 || desc.height == 0) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::InvalidDescriptor,
                                                "create_swapchain: the surface has no pixels; create it after the window has a size");
    }
    if (desc.buffer_count < kMinBufferCount || desc.buffer_count > kMaxBufferCount) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::OutOfRange, "create_swapchain: buffer_count must be between 2 and 16");
    }
    if (desc.format != Format::B8G8R8A8Unorm && desc.format != Format::R8G8B8A8Unorm) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::InvalidDescriptor,
                                                "create_swapchain: the format must be B8G8R8A8Unorm or R8G8B8A8Unorm");
    }
    if (!device.has_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME) || !device.has_instance_extension(kPlatformSurfaceExtension)) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::Unsupported,
                                                "create_swapchain: the instance was created without the surface extensions");
    }
    if (!device.has_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        return fail<std::unique_ptr<Swapchain>>(RhiErrorCode::Unsupported, "create_swapchain: the device lacks VK_KHR_swapchain");
    }

    NativeWindow window;
    window.hinstance = desc.hinstance;
    window.hwnd = desc.hwnd;
    window.display = desc.x11_display;
    window.window = desc.x11_window;
    auto swapchain = std::unique_ptr<VulkanSwapchain>(new VulkanSwapchain(device, window, desc.format, desc.buffer_count, desc.present,
                                                                         desc.width, desc.height));
    if (RhiStatus status = swapchain->initialize(); !status.ok()) {
        return core::failure(status.error());
    }
    std::unique_ptr<Swapchain> result = std::move(swapchain);
    return core::success(std::move(result));
}

RhiStatus VulkanSwapchain::initialize() {
    vk_format_ = to_vk(format_);
    if (RhiStatus status = create_surface(); !status.ok()) {
        return status;
    }
    const Api& api = device_.api();
    const VkPhysicalDevice physical = device_.physical_device();
    const std::uint32_t family = device_.queue_family(QueueType::Graphics);

    VkBool32 presentable = VK_FALSE;
    if (const VkResult result = api.vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface_, &presentable); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkGetPhysicalDeviceSurfaceSupportKHR", result));
    }
    if (presentable != VK_TRUE) {
        return fail(RhiErrorCode::Unsupported, "create_swapchain: the graphics queue family cannot present to this surface");
    }

    std::uint32_t format_count = 0;
    if (const VkResult result = api.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &format_count, nullptr); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkGetPhysicalDeviceSurfaceFormatsKHR", result));
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (format_count > 0) {
        if (const VkResult result = api.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &format_count, formats.data());
            result != VK_SUCCESS) {
            return core::failure(vk_error("create_swapchain: vkGetPhysicalDeviceSurfaceFormatsKHR", result));
        }
    }
    const bool has_format = std::any_of(formats.begin(), formats.end(), [this](const VkSurfaceFormatKHR& candidate) {
        return candidate.format == vk_format_ && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (!has_format) {
        return fail(RhiErrorCode::Unsupported, "create_swapchain: the surface does not offer the requested format in sRGB non-linear space");
    }

    std::uint32_t mode_count = 0;
    if (const VkResult result = api.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &mode_count, nullptr); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkGetPhysicalDeviceSurfacePresentModesKHR", result));
    }
    std::vector<VkPresentModeKHR> modes(mode_count);
    if (mode_count > 0) {
        if (const VkResult result = api.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &mode_count, modes.data());
            result != VK_SUCCESS) {
            return core::failure(vk_error("create_swapchain: vkGetPhysicalDeviceSurfacePresentModesKHR", result));
        }
    }
    // Tearing is immediate presentation. FIFO is required by the specification, so it is the fallback whenever tearing is not asked for.
    const bool immediate_supported = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end();
    const bool tearing = presents_with_tearing(requested_present_.vsync, requested_present_.allow_tearing, immediate_supported);
    settings_.vsync = !tearing;
    settings_.allow_tearing = tearing;
    present_mode_ = tearing ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;

    // Registered once here. Rebuilds keep the registration, and the destructor removes it.
    device_.register_swapchain(this);
    return build(VK_NULL_HANDLE);
}

RhiStatus VulkanSwapchain::create_surface() {
    const Api& api = device_.api();
#ifdef _WIN32
    if (api.vkCreateWin32SurfaceKHR == nullptr) {
        return fail(RhiErrorCode::Unsupported, "create_swapchain: vkCreateWin32SurfaceKHR is not available");
    }
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = static_cast<HINSTANCE>(native_.hinstance);
    info.hwnd = static_cast<HWND>(native_.hwnd);
    if (const VkResult result = api.vkCreateWin32SurfaceKHR(device_.instance(), &info, nullptr, &surface_); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkCreateWin32SurfaceKHR", result));
    }
#else
    if (api.vkCreateXlibSurfaceKHR == nullptr) {
        return fail(RhiErrorCode::Unsupported, "create_swapchain: vkCreateXlibSurfaceKHR is not available");
    }
    XlibSurfaceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.display = native_.display;
    info.window = static_cast<unsigned long>(native_.window);
    if (const VkResult result = api.vkCreateXlibSurfaceKHR(device_.instance(), &info, nullptr, &surface_); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkCreateXlibSurfaceKHR", result));
    }
#endif
    return core::success();
}

// Creates the swapchain at the current size, adopts its images as back buffers, and moves each image to Present. `previous`, when set,
// is retired once the new swapchain exists, as the specification requires.
RhiStatus VulkanSwapchain::build(VkSwapchainKHR previous) {
    const Api& api = device_.api();
    const VkPhysicalDevice physical = device_.physical_device();
    VkSurfaceCapabilitiesKHR caps{};
    if (const VkResult result = api.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface_, &caps); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
    }
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        return fail(RhiErrorCode::Unsupported, "create_swapchain: the surface cannot be read back (no TRANSFER_SRC usage)");
    }

    // Win32 and X11 surfaces report the window's size as their only legal extent, so the swapchain follows it. A surface that
    // reports no fixed extent takes the requested size, clamped to the limits.
    std::uint32_t extent_width = clamp_extent(width_, caps.minImageExtent.width, caps.maxImageExtent.width);
    std::uint32_t extent_height = clamp_extent(height_, caps.minImageExtent.height, caps.maxImageExtent.height);
    constexpr std::uint32_t kUndefinedExtent = 0xFFFFFFFFu;
    if (caps.currentExtent.width != kUndefinedExtent) {
        extent_width = caps.currentExtent.width;
        extent_height = caps.currentExtent.height;
    }
    if (extent_width == 0 || extent_height == 0) {
        // The window has no pixels (minimized). Keep the surface empty; a later resize with a real size builds it.
        width_ = 0;
        height_ = 0;
        return core::success();
    }
    std::uint32_t image_count = std::max(requested_buffers_, caps.minImageCount);
    if (caps.maxImageCount != 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info{};

    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = vk_format_;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = VkExtent2D{extent_width, extent_height};
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = pick_composite_alpha(caps.supportedCompositeAlpha);
    info.presentMode = present_mode_;
    info.clipped = VK_TRUE;
    info.oldSwapchain = previous;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (const VkResult result = api.vkCreateSwapchainKHR(device_.handle(), &info, nullptr, &created); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkCreateSwapchainKHR", result));
    }
    if (previous != VK_NULL_HANDLE) {
        api.vkDestroySwapchainKHR(device_.handle(), previous, nullptr);
    }
    swapchain_ = created;
    width_ = extent_width;
    height_ = extent_height;

    std::uint32_t images_count = 0;
    if (const VkResult result = api.vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &images_count, nullptr); result != VK_SUCCESS) {
        return core::failure(vk_error("create_swapchain: vkGetSwapchainImagesKHR", result));
    }
    std::vector<VkImage> images(images_count);
    if (images_count > 0) {
        if (const VkResult result = api.vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &images_count, images.data());
            result != VK_SUCCESS) {
            return core::failure(vk_error("create_swapchain: vkGetSwapchainImagesKHR", result));
        }
    }
    images.resize(images_count);

    for (std::uint32_t index = 0; index < images.size(); ++index) {
        TextureDesc desc;
        desc.width = width_;
        desc.height = height_;
        desc.mip_levels = 1;
        desc.format = format_;
        desc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource | TextureUsage::CopyDestination;
        desc.debug_name = "swapchain back buffer " + std::to_string(index);
        RhiResult<TextureHandle> handle = device_.adopt_back_buffer(images[index], desc);
        if (!handle.ok()) {
            release_images();
            return core::failure(handle.error());
        }
        buffers_.push_back(handle.value());
    }

    // A back buffer starts in Present, but its image is still UNDEFINED. The first move out of UNDEFINED, or into Present, happens after
    // the image is acquired: in the frame's list, or in the frame sync that present submits. Moving the image here, before any
    // acquire, is a layout transition on a presentable image the swapchain does not own yet.
    if (RhiStatus synced = create_sync_objects(); !synced.ok()) {
        release_images();
        destroy_sync_objects();
        return synced;
    }
    acquire_slot_ = 0;
    current_ = 0;
    needs_rebuild_ = false;
    return core::success();
}

// One acquire semaphore more than there are images, so the one a new acquire signals was waited on by an earlier frame.
RhiStatus VulkanSwapchain::create_sync_objects() {
    const Api& api = device_.api();
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    acquire_semaphores_.assign(buffers_.size() + 1, VK_NULL_HANDLE);
    render_done_.assign(buffers_.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : acquire_semaphores_) {
        if (const VkResult result = api.vkCreateSemaphore(device_.handle(), &info, nullptr, &semaphore); result != VK_SUCCESS) {
            return core::failure(vk_error("create_swapchain: vkCreateSemaphore", result));
        }
    }
    for (VkSemaphore& semaphore : render_done_) {
        if (const VkResult result = api.vkCreateSemaphore(device_.handle(), &info, nullptr, &semaphore); result != VK_SUCCESS) {
            return core::failure(vk_error("create_swapchain: vkCreateSemaphore", result));
        }
    }
    return core::success();
}

void VulkanSwapchain::release_images() {
    for (const TextureHandle& handle : buffers_) {
        static_cast<void>(device_.release_back_buffer(handle));
    }
    buffers_.clear();
}

void VulkanSwapchain::destroy_sync_objects() {
    const Api& api = device_.api();
    for (VkSemaphore semaphore : acquire_semaphores_) {
        if (semaphore != VK_NULL_HANDLE) {
            api.vkDestroySemaphore(device_.handle(), semaphore, nullptr);
        }
    }
    for (VkSemaphore semaphore : render_done_) {
        if (semaphore != VK_NULL_HANDLE) {
            api.vkDestroySemaphore(device_.handle(), semaphore, nullptr);
        }
    }
    acquire_semaphores_.clear();
    render_done_.clear();
    pending_acquire_ = VK_NULL_HANDLE;
    acquire_pending_ = false;
}

// The device must be idle. Retires the current images and syncs, then builds the swapchain again, passing the old one so the driver can
// reuse it. On failure the images are gone, so acquire returns nothing until a later resize rebuilds the swapchain.
RhiStatus VulkanSwapchain::recreate(std::uint32_t width, std::uint32_t height) {
    if (RhiStatus idle = device_.wait_idle(); !idle.ok()) {
        return idle;
    }
    release_images();
    destroy_sync_objects();
    width_ = width;
    height_ = height;
    RhiStatus status = build(swapchain_);
    if (!status.ok()) {
        release_images();
        destroy_sync_objects();
        width_ = 0;
        height_ = 0;
    }
    return status;
}

// A lost surface is recreated from the native window, which outlives it. The swapchain is built again at the current size.
RhiStatus VulkanSwapchain::rebuild_surface() {
    if (RhiStatus created = create_surface(); !created.ok()) {
        return created;
    }
    surface_lost_ = false;
    return build(VK_NULL_HANDLE);
}

void VulkanSwapchain::drop_surface() {
    static_cast<void>(device_.wait_idle());
    release_images();
    destroy_sync_objects();
    const Api& api = device_.api();
    if (swapchain_ != VK_NULL_HANDLE) {
        api.vkDestroySwapchainKHR(device_.handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        api.vkDestroySurfaceKHR(device_.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    surface_lost_ = true;
}

std::uint32_t VulkanSwapchain::width() const noexcept {
    return width_;
}

std::uint32_t VulkanSwapchain::height() const noexcept {
    return height_;
}

PresentSettings VulkanSwapchain::present_settings() const noexcept {
    return settings_;
}

RhiResult<std::optional<TextureHandle>> VulkanSwapchain::acquire_back_buffer() {
    if (acquired_) {
        return fail<std::optional<TextureHandle>>(RhiErrorCode::InvalidState,
                                                  "acquire_back_buffer: the previous back buffer was not presented");
    }
    if (width_ == 0 || height_ == 0) {
        return core::success(std::optional<TextureHandle>{});
    }
    if (surface_lost_) {
        if (RhiStatus rebuilt = rebuild_surface(); !rebuilt.ok()) {
            return core::failure(rebuilt.error());
        }
    }
    if (needs_rebuild_) {
        if (RhiStatus rebuilt = recreate(width_, height_); !rebuilt.ok()) {
            return core::failure(rebuilt.error());
        }
    }
    const Api& api = device_.api();
    // A second attempt follows an out-of-date result, which the rebuild answers.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (buffers_.empty()) {
            return core::success(std::optional<TextureHandle>{});
        }
        const VkSemaphore acquire = acquire_semaphores_[acquire_slot_];
        std::uint32_t index = 0;
        const VkResult result = api.vkAcquireNextImageKHR(device_.handle(), swapchain_, std::numeric_limits<std::uint64_t>::max(), acquire,
                                                          VK_NULL_HANDLE, &index);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            if (result == VK_SUBOPTIMAL_KHR) {
                needs_rebuild_ = true;
            }
            current_ = index;
            pending_acquire_ = acquire;
            acquire_slot_ = (acquire_slot_ + 1) % static_cast<std::uint32_t>(acquire_semaphores_.size());
            acquired_ = true;
            acquire_pending_ = true;
            return core::success(std::optional<TextureHandle>{buffers_[current_]});
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            if (RhiStatus rebuilt = recreate(width_, height_); !rebuilt.ok()) {
                return core::failure(rebuilt.error());
            }
            continue;
        }
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            drop_surface();
            return core::success(std::optional<TextureHandle>{});
        }
        return core::failure(vk_error("acquire_back_buffer: vkAcquireNextImageKHR", result));
    }
    return fail<std::optional<TextureHandle>>(RhiErrorCode::InvalidState,
                                              "acquire_back_buffer: the swapchain stayed out of date after a rebuild");
}

RhiStatus VulkanSwapchain::present() {
    if (!acquired_) {
        return fail(RhiErrorCode::InvalidState, "present: no back buffer was acquired for this frame");
    }
    const TextureObject* back = device_.find_texture(buffers_[current_]);
    if (back == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "present: the back buffer handle is no longer live");
    }
    if (back->state != ResourceState::Present) {
        return fail(RhiErrorCode::StateMismatch, "present: the back buffer must be in the Present state; transition it before presenting");
    }
    if (acquire_pending_) {
        // No list was submitted for this frame. Submit the sync alone, so the image is still ordered before presentation. An image no
        // list has moved out of UNDEFINED yet is moved to Present in that same submission, after the acquire wait.
        const bool first_use = back->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImage image = back->image;
        const auto to_present = [&api = device_.api(), image](VkCommandBuffer command) {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = 0;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            api.vkCmdPipelineBarrier2KHR(command, &dependency);
        };
        if (RhiStatus submitted = device_.submit_frame_syncs(first_use ? std::function<void(VkCommandBuffer)>(to_present)
                                                                       : std::function<void(VkCommandBuffer)>{});
            !submitted.ok()) {
            return submitted;
        }
        if (first_use) {
            if (TextureObject* texture = device_.find_texture(buffers_[current_])) {
                texture->layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            }
        }
    } else if (back->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        // A list already took this frame's acquire and left the image untouched, so no ordering is left to add here.
        return fail(RhiErrorCode::StateMismatch, "present: the back buffer was never transitioned; transition it before presenting");
    }

    const Api& api = device_.api();
    VkSemaphore render_done = render_done_[current_];
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_done;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &current_;
    const VkResult result = api.vkQueuePresentKHR(device_.queue_handle(QueueType::Graphics), &info);
    acquired_ = false;
    if (result == VK_SUCCESS) {
        return core::success();
    }
    // The image was not shown, or was shown with a stale size. Either way the next acquire rebuilds the swapchain first.
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_rebuild_ = true;
        return core::success();
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        drop_surface();
        return core::success();
    }
    return core::failure(vk_error("present: vkQueuePresentKHR", result));
}

RhiStatus VulkanSwapchain::resize(std::uint32_t width, std::uint32_t height) {
    if (acquired_) {
        return fail(RhiErrorCode::InvalidState, "resize: a back buffer is acquired; present it before resizing");
    }
    if (width == 0 || height == 0) {
        // Minimized: keep the swapchain, and acquire returns nothing until a real size arrives.
        width_ = 0;
        height_ = 0;
        return core::success();
    }
    if (width == width_ && height == height_ && !needs_rebuild_ && !surface_lost_ && !buffers_.empty()) {
        return core::success();
    }
    width_ = width;
    height_ = height;
    if (surface_lost_) {
        return rebuild_surface();
    }
    return recreate(width, height);
}

bool VulkanSwapchain::add_frame_sync(SubmitSync& sync) const {
    if (!acquire_pending_) {
        return false;
    }
    sync.waits.push_back(pending_acquire_);
    sync.wait_stages.push_back(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    sync.wait_values.push_back(0);
    sync.signals.push_back(render_done_[current_]);
    sync.signal_values.push_back(0);
    return true;
}

void VulkanSwapchain::frame_sync_submitted() {
    acquire_pending_ = false;
}

}  // namespace engine::rhi::vulkan
