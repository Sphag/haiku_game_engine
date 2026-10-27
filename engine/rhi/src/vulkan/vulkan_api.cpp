#include "vulkan/vulkan_api.hpp"

#include "engine/core/result.hpp"

#include <string>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace engine::rhi::vulkan {
namespace {

#ifdef _WIN32
constexpr const char* kLoaderName = "vulkan-1.dll";
#else
constexpr const char* kLoaderName = "libvulkan.so.1";
#endif

}  // namespace

VulkanLibrary::~VulkanLibrary() {
    if (module_ != nullptr) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(module_));
#else
        dlclose(module_);
#endif
    }
}

RhiStatus VulkanLibrary::open() {
#ifdef _WIN32
    HMODULE module = LoadLibraryA(kLoaderName);
    if (module == nullptr) {
        return fail(RhiErrorCode::Unsupported, std::string("create_device: the Vulkan loader ") + kLoaderName + " is not installed");
    }
    module_ = module;
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
#else
    void* module = dlopen(kLoaderName, RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        const char* reason = dlerror();
        return fail(RhiErrorCode::Unsupported, std::string("create_device: the Vulkan loader ") + kLoaderName + " is not available: " +
                                                   (reason != nullptr ? reason : "unknown error"));
    }
    module_ = module;
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(module, "vkGetInstanceProcAddr"));
#endif
    if (get == nullptr) {
        return fail(RhiErrorCode::Unsupported, "create_device: the Vulkan loader does not export vkGetInstanceProcAddr");
    }
    get_instance_proc_addr_ = get;
    return core::success();
}

namespace {

// Looks one name up through the loader. Returns the typed pointer, or nullptr when the loader does not know it.
template <typename Pfn>
Pfn lookup(PFN_vkGetInstanceProcAddr get, VkInstance instance, const char* name) {
    return reinterpret_cast<Pfn>(get(instance, name));
}

}  // namespace

RhiStatus load_global_functions(Api& api, PFN_vkGetInstanceProcAddr get_proc) {
#define ENGINE_VK_LOAD_GLOBAL(name)                                             \
    api.name = lookup<PFN_##name>(get_proc, VK_NULL_HANDLE, #name);             \
    if (api.name == nullptr) {                                                  \
        return fail(RhiErrorCode::Unsupported, "create_device: the Vulkan loader lacks " #name); \
    }
    ENGINE_VK_GLOBAL_FUNCTIONS(ENGINE_VK_LOAD_GLOBAL)
#undef ENGINE_VK_LOAD_GLOBAL
    return core::success();
}

RhiStatus load_instance_functions(Api& api, PFN_vkGetInstanceProcAddr get_proc, VkInstance instance) {
#define ENGINE_VK_LOAD_CORE(name)                                                        \
    api.name = lookup<PFN_##name>(get_proc, instance, #name);                            \
    if (api.name == nullptr) {                                                           \
        return fail(RhiErrorCode::Unsupported, "create_device: the Vulkan driver lacks " #name); \
    }
    ENGINE_VK_INSTANCE_FUNCTIONS(ENGINE_VK_LOAD_CORE)
    ENGINE_VK_DEVICE_FUNCTIONS(ENGINE_VK_LOAD_CORE)
#undef ENGINE_VK_LOAD_CORE

#define ENGINE_VK_LOAD_OPTIONAL(name) api.name = lookup<PFN_##name>(get_proc, instance, #name);
    ENGINE_VK_SURFACE_FUNCTIONS(ENGINE_VK_LOAD_OPTIONAL)
    ENGINE_VK_SWAPCHAIN_FUNCTIONS(ENGINE_VK_LOAD_OPTIONAL)
    ENGINE_VK_DEBUG_FUNCTIONS(ENGINE_VK_LOAD_OPTIONAL)
    ENGINE_VK_SYNC2_FUNCTIONS(ENGINE_VK_LOAD_OPTIONAL)
    ENGINE_VK_MESH_FUNCTIONS(ENGINE_VK_LOAD_OPTIONAL)
    ENGINE_VK_RAY_TRACING_FUNCTIONS(ENGINE_VK_LOAD_OPTIONAL)
#undef ENGINE_VK_LOAD_OPTIONAL
#ifdef _WIN32
    api.vkCreateWin32SurfaceKHR = lookup<PFN_vkCreateWin32SurfaceKHR>(get_proc, instance, "vkCreateWin32SurfaceKHR");
#else
    api.vkCreateXlibSurfaceKHR = lookup<PFN_vkCreateXlibSurfaceKHR_local>(get_proc, instance, "vkCreateXlibSurfaceKHR");
#endif
    return core::success();
}

std::string result_text(VkResult result) {
    std::string name;
    switch (result) {
        case VK_SUCCESS:
            name = "VK_SUCCESS";
            break;
        case VK_NOT_READY:
            name = "VK_NOT_READY";
            break;
        case VK_TIMEOUT:
            name = "VK_TIMEOUT";
            break;
        case VK_SUBOPTIMAL_KHR:
            name = "VK_SUBOPTIMAL_KHR";
            break;
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            name = "VK_ERROR_OUT_OF_HOST_MEMORY";
            break;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            name = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            break;
        case VK_ERROR_INITIALIZATION_FAILED:
            name = "VK_ERROR_INITIALIZATION_FAILED";
            break;
        case VK_ERROR_DEVICE_LOST:
            name = "VK_ERROR_DEVICE_LOST";
            break;
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            name = "VK_ERROR_EXTENSION_NOT_PRESENT";
            break;
        case VK_ERROR_FEATURE_NOT_PRESENT:
            name = "VK_ERROR_FEATURE_NOT_PRESENT";
            break;
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            name = "VK_ERROR_INCOMPATIBLE_DRIVER";
            break;
        case VK_ERROR_TOO_MANY_OBJECTS:
            name = "VK_ERROR_TOO_MANY_OBJECTS";
            break;
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            name = "VK_ERROR_FORMAT_NOT_SUPPORTED";
            break;
        case VK_ERROR_FRAGMENTED_POOL:
            name = "VK_ERROR_FRAGMENTED_POOL";
            break;
        case VK_ERROR_OUT_OF_DATE_KHR:
            name = "VK_ERROR_OUT_OF_DATE_KHR";
            break;
        case VK_ERROR_SURFACE_LOST_KHR:
            name = "VK_ERROR_SURFACE_LOST_KHR";
            break;
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            name = "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
            break;
        default:
            break;
    }
    const std::string number = std::to_string(static_cast<std::int32_t>(result));
    return name.empty() ? number : name + " (" + number + ")";
}

RhiError vk_error(const char* operation, VkResult result) {
    const RhiErrorCode code = result == VK_ERROR_DEVICE_LOST ? RhiErrorCode::DeviceLost : RhiErrorCode::InvalidState;
    return RhiError{code, std::string(operation) + " failed with " + result_text(result)};
}

}  // namespace engine::rhi::vulkan
