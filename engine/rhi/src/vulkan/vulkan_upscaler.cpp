#include "vulkan/vulkan_upscaler.hpp"

#include "fsr2_common.hpp"
#include "vulkan/vulkan_common.hpp"

#include "ffx_fsr2.h"
#include "vk/ffx_fsr2_vk.h"

// FSR 2's Vulkan backend calls five instance-level functions directly, not through its function table. This engine loads Vulkan at run
// time and links no loader, so those names are defined here and forward to the device's loaded entry points. The pointer is set for the
// span of each FSR 2 call (ShimScope); the device is used from one thread, so one pointer is enough.
static const engine::rhi::vulkan::Api* g_shim_api = nullptr;

// The shims carry the exact signatures of vulkan.h and the C linkage that FSR 2's calls expect, so they sit at global scope.
extern "C" {

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    g_shim_api->vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
    g_shim_api->vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
    g_shim_api->vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                              VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    g_shim_api->vkGetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName,
                                                                   uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    return g_shim_api->vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

}  // extern "C"

namespace engine::rhi::vulkan {
namespace {

class ShimScope {
public:
    explicit ShimScope(const Api* api) : previous_(g_shim_api) {
        g_shim_api = api;
    }
    ~ShimScope() {
        g_shim_api = previous_;
    }
    ShimScope(const ShimScope&) = delete;
    ShimScope& operator=(const ShimScope&) = delete;

private:
    const Api* previous_;
};

}  // namespace

VulkanUpscaler::VulkanUpscaler(VulkanDevice& device, const UpscalerDesc& desc)
    : desc_(desc), device_id_(device.id()), owner_(device.ref()), physical_(device.physical_device()) {}

VulkanUpscaler::~VulkanUpscaler() {
    if (context_) {
        ShimScope scope(owner_.api);
        // FSR 2 may still have work in flight in the command buffers it recorded, so the device drains before its context goes.
        owner_.api->vkDeviceWaitIdle(owner_.device);
        ffxFsr2ContextDestroy(context_.get());
    }
}

RhiStatus VulkanUpscaler::initialize(bool debug_checks) {
    ShimScope scope(owner_.api);
    const std::size_t scratch_bytes = ffxFsr2GetScratchMemorySizeVK(physical_);
    scratch_.assign((scratch_bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
    const FfxErrorCode interface_status = ffxFsr2GetInterfaceVK(&interface_, scratch_.data(), scratch_.size() * sizeof(std::uint64_t),
                                                                physical_, owner_.api->vkGetDeviceProcAddr);
    if (interface_status != FFX_OK) {
        return fail(RhiErrorCode::Unsupported, "create_upscaler: FSR 2 cannot use this Vulkan device (interface error)");
    }
    const FfxFsr2ContextDescription description =
        fsr2::context_description(desc_, debug_checks, interface_, ffxGetDeviceVK(owner_.device));
    auto context = std::make_unique<FfxFsr2Context>();
    const FfxErrorCode status = ffxFsr2ContextCreate(context.get(), &description);
    if (status != FFX_OK) {
        return fail(RhiErrorCode::Unsupported, "create_upscaler: FSR 2 context creation failed on this Vulkan device");
    }
    context_ = std::move(context);
    return core::success();
}

FfxResource VulkanUpscaler::wrap(TextureObject& texture, const wchar_t* name, FfxResourceStates state) {
    return ffxGetTextureResourceVK(context_.get(), texture.image, texture.view, texture.desc.width, texture.desc.height,
                                   to_vk(texture.desc.format), name, state);
}

RhiStatus VulkanUpscaler::record(VkCommandBuffer command, const UpscaleImages& images, const UpscaleDispatch& dispatch) {
    ShimScope scope(owner_.api);
    const std::uint32_t errors_before = fsr2::error_count();
    fsr2::Resources resources;
    resources.color = wrap(*images.color, L"fsr2 color", FFX_RESOURCE_STATE_COMPUTE_READ);
    resources.depth = wrap(*images.depth, L"fsr2 depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    resources.motion = wrap(*images.motion, L"fsr2 motion", FFX_RESOURCE_STATE_COMPUTE_READ);
    if (images.reactive != nullptr) {
        resources.reactive = wrap(*images.reactive, L"fsr2 reactive", FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    if (images.exposure != nullptr) {
        resources.exposure = wrap(*images.exposure, L"fsr2 exposure", FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    resources.output = wrap(*images.output, L"fsr2 output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxFsr2DispatchDescription params = fsr2::dispatch_description(dispatch, resources, ffxGetCommandListVK(command));
    const FfxErrorCode status = ffxFsr2ContextDispatch(context_.get(), &params);
    if (status != FFX_OK || fsr2::error_count() != errors_before) {
        return fail(RhiErrorCode::InvalidState, "upscale: FSR 2 reported an error for this dispatch (see the FSR 2 messages)");
    }
    return core::success();
}

}  // namespace engine::rhi::vulkan
