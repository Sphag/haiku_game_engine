#pragma once

// Vulkan entry points for the Vulkan backend (US-028). The loader is opened at run time (libvulkan.so.1 or vulkan-1.dll), so no
// Vulkan import library is linked, and the Vulkan-Headers this backend compiles against are vendored under third_party/. Nothing
// here leaves engine/rhi/src: the public RHI headers stay free of Vulkan types.

#include "engine/rhi/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <string>

namespace engine::rhi::vulkan {

#ifndef _WIN32
// VkXlibSurfaceCreateInfoKHR without X11 headers. The layout is the Vulkan ABI: Display* and the Window XID are the only fields.
struct XlibSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* display;
    unsigned long window;
};
using PFN_vkCreateXlibSurfaceKHR_local = VkResult(VKAPI_PTR*)(VkInstance, const XlibSurfaceCreateInfo*, const VkAllocationCallbacks*,
                                                              VkSurfaceKHR*);
#endif

// Every entry point the backend calls. Core functions are required once the loader opens; extension functions are null until
// their extension is enabled, and each call site checks its own.
#define ENGINE_VK_GLOBAL_FUNCTIONS(X)              \
    X(vkEnumerateInstanceVersion)                  \
    X(vkEnumerateInstanceExtensionProperties)      \
    X(vkEnumerateInstanceLayerProperties)          \
    X(vkCreateInstance)

#define ENGINE_VK_INSTANCE_FUNCTIONS(X)            \
    X(vkDestroyInstance)                           \
    X(vkEnumeratePhysicalDevices)                  \
    X(vkGetPhysicalDeviceProperties)               \
    X(vkGetPhysicalDeviceFeatures2)                \
    X(vkGetPhysicalDeviceProperties2)              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)    \
    X(vkGetPhysicalDeviceMemoryProperties)         \
    X(vkEnumerateDeviceExtensionProperties)        \
    X(vkGetDeviceProcAddr)                         \
    X(vkCreateDevice)

#define ENGINE_VK_DEVICE_FUNCTIONS(X)              \
    X(vkDestroyDevice)                             \
    X(vkGetDeviceQueue)                            \
    X(vkDeviceWaitIdle)                            \
    X(vkCreateCommandPool)                         \
    X(vkDestroyCommandPool)                        \
    X(vkAllocateCommandBuffers)                    \
    X(vkBeginCommandBuffer)                        \
    X(vkEndCommandBuffer)                          \
    X(vkCreateSemaphore)                           \
    X(vkDestroySemaphore)                          \
    X(vkGetSemaphoreCounterValue)                  \
    X(vkWaitSemaphores)                            \
    X(vkCreateBuffer)                              \
    X(vkDestroyBuffer)                             \
    X(vkGetBufferMemoryRequirements)               \
    X(vkBindBufferMemory)                          \
    X(vkCreateImage)                               \
    X(vkDestroyImage)                              \
    X(vkGetImageMemoryRequirements)                \
    X(vkBindImageMemory)                           \
    X(vkCreateImageView)                           \
    X(vkDestroyImageView)                          \
    X(vkAllocateMemory)                            \
    X(vkFreeMemory)                                \
    X(vkMapMemory)                                 \
    X(vkUnmapMemory)                               \
    X(vkCreateQueryPool)                           \
    X(vkDestroyQueryPool)                          \
    X(vkGetQueryPoolResults)                       \
    X(vkCreateRenderPass)                          \
    X(vkDestroyRenderPass)                         \
    X(vkCreateFramebuffer)                         \
    X(vkDestroyFramebuffer)                        \
    X(vkCreateDescriptorSetLayout)                 \
    X(vkDestroyDescriptorSetLayout)                \
    X(vkCreateDescriptorPool)                      \
    X(vkDestroyDescriptorPool)                     \
    X(vkAllocateDescriptorSets)                    \
    X(vkFreeDescriptorSets)                        \
    X(vkUpdateDescriptorSets)                      \
    X(vkCreatePipelineLayout)                      \
    X(vkDestroyPipelineLayout)                     \
    X(vkCreateShaderModule)                        \
    X(vkDestroyShaderModule)                       \
    X(vkCreateComputePipelines)                    \
    X(vkCreateGraphicsPipelines)                   \
    X(vkDestroyPipeline)                           \
    X(vkCmdBeginRenderPass)                        \
    X(vkCmdEndRenderPass)                          \
    X(vkCmdBindPipeline)                           \
    X(vkCmdBindDescriptorSets)                     \
    X(vkCmdDispatch)                               \
    X(vkCmdDrawIndexed)                            \
    X(vkCmdDrawIndexedIndirect)                    \
    X(vkCmdDrawIndexedIndirectCount)               \
    X(vkCmdBindVertexBuffers)                      \
    X(vkCmdBindIndexBuffer)                        \
    X(vkCmdSetViewport)                            \
    X(vkCmdSetScissor)                             \
    X(vkCmdCopyBuffer)                             \
    X(vkCmdCopyImageToBuffer)                      \
    X(vkCmdCopyBufferToImage)                      \
    X(vkCmdResetQueryPool)                         \
    X(vkCmdWriteTimestamp)                         \
    X(vkGetBufferDeviceAddress)

// Extension functions. Null until the extension is enabled on the instance or device.
#define ENGINE_VK_SURFACE_FUNCTIONS(X)             \
    X(vkDestroySurfaceKHR)                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)        \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define ENGINE_VK_SWAPCHAIN_FUNCTIONS(X)           \
    X(vkCreateSwapchainKHR)                        \
    X(vkDestroySwapchainKHR)                       \
    X(vkGetSwapchainImagesKHR)                     \
    X(vkAcquireNextImageKHR)                       \
    X(vkQueuePresentKHR)

#define ENGINE_VK_DEBUG_FUNCTIONS(X)               \
    X(vkCreateDebugUtilsMessengerEXT)              \
    X(vkDestroyDebugUtilsMessengerEXT)

// VK_KHR_synchronization2 entry points (US-030). Null until the extension is enabled on the device; the device checks them at creation.
#define ENGINE_VK_SYNC2_FUNCTIONS(X)               \
    X(vkQueueSubmit2KHR)                           \
    X(vkCmdPipelineBarrier2KHR)

// VK_EXT_mesh_shader entry points (US-038). Null until the extension is enabled on the device; the device checks them at creation.
#define ENGINE_VK_MESH_FUNCTIONS(X)                \
    X(vkCmdDrawMeshTasksEXT)
// VK_KHR_acceleration_structure and VK_KHR_ray_tracing_pipeline entry points (US-042). Null until both extensions are enabled on the device.
#define ENGINE_VK_RAY_TRACING_FUNCTIONS(X)         \
    X(vkCreateAccelerationStructureKHR)            \
    X(vkDestroyAccelerationStructureKHR)           \
    X(vkGetAccelerationStructureBuildSizesKHR)     \
    X(vkGetAccelerationStructureDeviceAddressKHR)  \
    X(vkCmdBuildAccelerationStructuresKHR)         \
    X(vkCreateRayTracingPipelinesKHR)              \
    X(vkGetRayTracingShaderGroupHandlesKHR)        \
    X(vkCmdTraceRaysKHR)

#define ENGINE_VK_DECLARE(name) PFN_##name name = nullptr;

// Holds the resolved entry points. Its members are named after the Vulkan functions they hold.
struct Api {
    ENGINE_VK_GLOBAL_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_INSTANCE_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_DEVICE_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_SURFACE_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_SWAPCHAIN_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_DEBUG_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_SYNC2_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_MESH_FUNCTIONS(ENGINE_VK_DECLARE)
    ENGINE_VK_RAY_TRACING_FUNCTIONS(ENGINE_VK_DECLARE)
#ifdef _WIN32
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
#else
    PFN_vkCreateXlibSurfaceKHR_local vkCreateXlibSurfaceKHR = nullptr;
#endif
};

#undef ENGINE_VK_DECLARE

// Owns the loaded Vulkan library. The library stays open for the life of the object, so every entry point stays valid.
class VulkanLibrary {
public:
    VulkanLibrary() = default;
    ~VulkanLibrary();
    VulkanLibrary(const VulkanLibrary&) = delete;
    VulkanLibrary& operator=(const VulkanLibrary&) = delete;

    // Fails with Unsupported when the loader is not installed.
    [[nodiscard]] RhiStatus open();
    [[nodiscard]] PFN_vkGetInstanceProcAddr get_instance_proc_addr() const noexcept { return get_instance_proc_addr_; }

private:
    void* module_ = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
};

// Resolves the global functions. Fails with the name of a missing core function.
[[nodiscard]] RhiStatus load_global_functions(Api& api, PFN_vkGetInstanceProcAddr get_proc);
// Resolves the instance, core device, and extension functions for `instance`. Device functions go through the instance, which the
// loader dispatches to the driver. Extension entries stay null when the loader does not provide them. Call after instance creation.
[[nodiscard]] RhiStatus load_instance_functions(Api& api, PFN_vkGetInstanceProcAddr get_proc, VkInstance instance);

// "VK_RESULT_NAME (value)" style text for diagnostics. Unknown codes print only the number.
[[nodiscard]] std::string result_text(VkResult result);

// Maps a failed Vulkan call to an RHI error. Device loss becomes DeviceLost; everything else is InvalidState.
[[nodiscard]] RhiError vk_error(const char* operation, VkResult result);

}  // namespace engine::rhi::vulkan
