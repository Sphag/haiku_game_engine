#include "vulkan/vulkan_device.hpp"

#include "vulkan/vulkan_command_list.hpp"
#include "vulkan/vulkan_common.hpp"
#include "vulkan/vulkan_swapchain.hpp"
#if ENGINE_FSR2_AVAILABLE
#include "vulkan/vulkan_upscaler.hpp"
#endif

#include "engine/core/result.hpp"
#include "engine/rhi/pipeline_rules.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace engine::rhi::vulkan {
namespace {

// Device ids start high so a Vulkan handle never matches a DX12 or Validation device's id, even when both devices exist.
std::uint32_t next_device_id() {
    static std::atomic<std::uint32_t> next{0x50000000u};
    return next.fetch_add(1);
}

constexpr std::array<QueueType, kQueueCount> kQueueOrder{QueueType::Graphics, QueueType::Compute, QueueType::Copy};

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kSurfaceExtension = VK_KHR_SURFACE_EXTENSION_NAME;
constexpr const char* kDebugUtilsExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
constexpr const char* kSwapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
constexpr const char* kMeshShaderExtension = VK_EXT_MESH_SHADER_EXTENSION_NAME;
constexpr const char* kRayTracingExtension = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
constexpr const char* kAccelerationStructureExtension = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
constexpr const char* kDeferredHostOperationsExtension = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
// The SPIR-V library that DXC writes declares RayQuery, and the ray tracing pipeline's library interface needs pipeline libraries.
constexpr const char* kRayQueryExtension = VK_KHR_RAY_QUERY_EXTENSION_NAME;
constexpr const char* kPipelineLibraryExtension = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
constexpr const char* kSynchronization2Extension = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
constexpr const char* kMutableDescriptorExtension = VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME;
// The pool page size. A page holds this many sets and this many descriptors of each mutable or uniform kind.
constexpr std::uint32_t kPoolSets = 256;
constexpr std::uint32_t kPoolDescriptors = 1024;
// Every type a mutable descriptor may hold in a pool page: the union of the layouts' mutable lists. The acceleration structure type joins
// when the device has ray tracing, the same rule the layouts follow.
constexpr std::array<VkDescriptorType, 3> kPoolMutableTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
#ifdef _WIN32
constexpr const char* kPlatformSurfaceExtension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
constexpr const char* kPlatformSurfaceExtension = "VK_KHR_xlib_surface";
#endif

// Buffers that ray tracing reads by device address (geometry and instance inputs, the shader table, the scratch). A device with ray tracing
// gives every buffer these flags, so any buffer can serve as an input. Memory for such a buffer must carry the device address bit.
constexpr VkBufferUsageFlags kRayTracingBufferUsage =
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

VkMemoryAllocateFlagsInfo device_address_flags() {
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    return flags;
}

bool contains(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::vector<std::string> instance_extension_names(const Api& api) {
    std::uint32_t count = 0;
    std::vector<std::string> names;
    if (api.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return names;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (count > 0 && api.vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()) != VK_SUCCESS) {
        return names;
    }
    for (const VkExtensionProperties& property : properties) {
        names.emplace_back(property.extensionName);
    }
    return names;
}

std::vector<std::string> layer_names(const Api& api) {
    std::uint32_t count = 0;
    std::vector<std::string> names;
    if (api.vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return names;
    }
    std::vector<VkLayerProperties> properties(count);
    if (count > 0 && api.vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) {
        return names;
    }
    for (const VkLayerProperties& property : properties) {
        names.emplace_back(property.layerName);
    }
    return names;
}

// One physical device that can host this backend, with what the selection and the capability report need.
struct Candidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::vector<std::string> extensions;
    std::uint32_t graphics_family = 0;
    std::uint32_t graphics_timestamp_bits = 0;
    bool timeline_semaphores = false;
    bool synchronization2 = false;
    bool mutable_descriptors = false;
    bool draw_indirect_count = false;
    bool mesh_shaders = false;
    bool ray_tracing = false;

    // Hardware ranks first, software (the CPU device type) last, the same order DX12 uses for WARP.
    [[nodiscard]] int rank() const noexcept {
        switch (properties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                return 0;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                return 1;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                return 2;
            case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                return 3;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                return 4;
            default:
                return 3;
        }
    }
    [[nodiscard]] bool software() const noexcept { return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU; }
};

// Reads one physical device. Returns nothing for a device that cannot host the backend: no Vulkan 1.2, no swapchain extension, no
// graphics queue, no timeline semaphores, or no synchronization2.
bool describe(const Api& api, VkPhysicalDevice device, Candidate& out) {
    Candidate candidate;
    candidate.device = device;
    api.vkGetPhysicalDeviceProperties(device, &candidate.properties);
    if (candidate.properties.apiVersion < VK_API_VERSION_1_2) {
        return false;
    }

    std::uint32_t extension_count = 0;
    if (api.vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0 && api.vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& extension : extensions) {
        candidate.extensions.emplace_back(extension.extensionName);
    }
    if (!contains(candidate.extensions, kSwapchainExtension)) {
        return false;
    }

    std::uint32_t family_count = 0;
    api.vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    api.vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
    bool found_graphics = false;
    for (std::uint32_t index = 0; index < family_count && !found_graphics; ++index) {
        if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[index].queueCount > 0) {
            candidate.graphics_family = index;
            candidate.graphics_timestamp_bits = families[index].timestampValidBits;
            found_graphics = true;
        }
    }
    if (!found_graphics) {
        return false;
    }

    // Synchronization2 and mutable descriptors are extension features, so their structs join the chain only when the extension exists.
    candidate.synchronization2 = contains(candidate.extensions, kSynchronization2Extension);
    candidate.mutable_descriptors = contains(candidate.extensions, kMutableDescriptorExtension);
    VkPhysicalDeviceSynchronization2Features sync2{};
    sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_features{};
    mutable_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
    if (candidate.mutable_descriptors) {
        sync2.pNext = &mutable_features;
    }
    VkPhysicalDeviceVulkan12Features v12{};

    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    if (candidate.synchronization2) {
        v12.pNext = &sync2;
    }
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &v12;
    api.vkGetPhysicalDeviceFeatures2(device, &features);
    candidate.timeline_semaphores = v12.timelineSemaphore == VK_TRUE;
    candidate.draw_indirect_count = v12.drawIndirectCount == VK_TRUE;
    if (!candidate.timeline_semaphores) {
        return false;
    }
    candidate.synchronization2 = candidate.synchronization2 && sync2.synchronization2 == VK_TRUE;
    if (!candidate.synchronization2) {
        return false;
    }
    candidate.mutable_descriptors = candidate.mutable_descriptors && mutable_features.mutableDescriptorType == VK_TRUE;

    // Mesh shaders need the task and mesh stages. The task stage writes the meshlet visibility counters, and a stage that writes buffers
    // needs the core feature vertexPipelineStoresAndAtomics.
    if (contains(candidate.extensions, kMeshShaderExtension)) {
        VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
        mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 mesh_features{};
        mesh_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        mesh_features.pNext = &mesh;
        api.vkGetPhysicalDeviceFeatures2(device, &mesh_features);
        candidate.mesh_shaders = mesh.meshShader == VK_TRUE && mesh.taskShader == VK_TRUE &&
                                 mesh_features.features.vertexPipelineStoresAndAtomics == VK_TRUE;
    }
    // Ray tracing (US-042) needs both KHR extensions, the deferred host operations they build on, and buffer device addresses for the
    // acceleration structure inputs. Its feature structs join one chain, so each is read from the same query.
    if (contains(candidate.extensions, kRayTracingExtension) && contains(candidate.extensions, kAccelerationStructureExtension) &&
        contains(candidate.extensions, kDeferredHostOperationsExtension) && contains(candidate.extensions, kRayQueryExtension) &&
        contains(candidate.extensions, kPipelineLibraryExtension)) {
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{};
        acceleration.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing{};
        ray_tracing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR ray_query{};
        ray_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        VkPhysicalDeviceVulkan12Features buffer_address{};
        buffer_address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        buffer_address.pNext = &ray_tracing;
        ray_tracing.pNext = &acceleration;
        acceleration.pNext = &ray_query;
        VkPhysicalDeviceFeatures2 ray_features{};
        ray_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        ray_features.pNext = &buffer_address;
        api.vkGetPhysicalDeviceFeatures2(device, &ray_features);
        candidate.ray_tracing = ray_tracing.rayTracingPipeline == VK_TRUE && acceleration.accelerationStructure == VK_TRUE &&
                                ray_query.rayQuery == VK_TRUE && buffer_address.bufferDeviceAddress == VK_TRUE;
    }
    out = std::move(candidate);
    return true;
}

// The queue family for one queue type. A dedicated family is preferred, so compute and copy work can overlap graphics where the
// driver offers it. Otherwise the type shares the family above it.
std::uint32_t pick_family(const std::vector<VkQueueFamilyProperties>& families, VkQueueFlags need, VkQueueFlags exclude,
                          std::uint32_t fallback) {
    for (std::uint32_t index = 0; index < families.size(); ++index) {
        if ((families[index].queueFlags & need) != 0 && (families[index].queueFlags & exclude) == 0 && families[index].queueCount > 0) {
            return index;
        }
    }
    return fallback;
}

}  // namespace

// ---- Object destructors. Each one runs only while its device is alive (see ~VulkanDevice). ----

BufferObject::~BufferObject() {
    if (owner.api == nullptr) {
        return;
    }
    if (mapped != nullptr) {
        owner.api->vkUnmapMemory(owner.device, memory);
    }
    if (buffer != VK_NULL_HANDLE) {
        owner.api->vkDestroyBuffer(owner.device, buffer, nullptr);
    }
    if (memory != VK_NULL_HANDLE) {
        owner.api->vkFreeMemory(owner.device, memory, nullptr);
    }
}

TextureObject::~TextureObject() {
    if (owner.api == nullptr) {
        return;
    }
    if (view != VK_NULL_HANDLE) {
        owner.api->vkDestroyImageView(owner.device, view, nullptr);
    }
    // A swapchain image has no memory of its own: the swapchain destroys the image.
    if (memory != VK_NULL_HANDLE) {
        owner.api->vkDestroyImage(owner.device, image, nullptr);
        owner.api->vkFreeMemory(owner.device, memory, nullptr);
    }
}

FenceObject::~FenceObject() {
    if (owner.api != nullptr && semaphore != VK_NULL_HANDLE) {
        owner.api->vkDestroySemaphore(owner.device, semaphore, nullptr);
    }
}

QueryPoolObject::~QueryPoolObject() {
    if (owner.api != nullptr && pool != VK_NULL_HANDLE) {
        owner.api->vkDestroyQueryPool(owner.device, pool, nullptr);
    }
}

DescriptorSetLayoutObject::~DescriptorSetLayoutObject() {
    if (owner.api != nullptr && layout != VK_NULL_HANDLE) {
        owner.api->vkDestroyDescriptorSetLayout(owner.device, layout, nullptr);
    }
}

DescriptorPoolObject::~DescriptorPoolObject() {
    if (owner.api != nullptr && pool != VK_NULL_HANDLE) {
        owner.api->vkDestroyDescriptorPool(owner.device, pool, nullptr);
    }
}

// Freeing a set returns its pool budget and its share of the device's shader-visible count. A set that never allocated has no handle
// and no cost, so it releases nothing.
DescriptorSetObject::~DescriptorSetObject() {
    if (owner.api == nullptr || set == VK_NULL_HANDLE || !pool) {
        return;
    }
    owner.api->vkFreeDescriptorSets(owner.device, pool->pool, 1, &set);
    pool->used.sets -= cost.sets;
    pool->used.uniform -= cost.uniform;
    pool->used.mutable_descriptors -= cost.mutable_descriptors;
    if (counters) {
        counters->shader_descriptors -= cost.uniform + cost.mutable_descriptors;
    }
}

AccelerationStructureObject::~AccelerationStructureObject() {
    if (owner.api == nullptr) {
        return;
    }
    // The members that hold the storage and the inputs are released after this, when the last reference to them goes.
    if (structure != VK_NULL_HANDLE) {
        owner.api->vkDestroyAccelerationStructureKHR(owner.device, structure, nullptr);
    }
}

RayTracingPipelineObject::~RayTracingPipelineObject() {
    if (owner.api == nullptr) {
        return;
    }
    if (pipeline != VK_NULL_HANDLE) {
        owner.api->vkDestroyPipeline(owner.device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        owner.api->vkDestroyPipelineLayout(owner.device, layout, nullptr);
    }
}

PipelineObject::~PipelineObject() {
    if (owner.api == nullptr) {
        return;
    }
    if (pipeline != VK_NULL_HANDLE) {
        owner.api->vkDestroyPipeline(owner.device, pipeline, nullptr);
    }
    if (render_pass != VK_NULL_HANDLE) {
        owner.api->vkDestroyRenderPass(owner.device, render_pass, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        owner.api->vkDestroyPipelineLayout(owner.device, layout, nullptr);
    }
}

ListData::~ListData() {
    if (owner.api == nullptr) {
        return;
    }
    for (VkFramebuffer framebuffer : framebuffers) {
        owner.api->vkDestroyFramebuffer(owner.device, framebuffer, nullptr);
    }
    for (VkRenderPass render_pass : render_passes) {
        owner.api->vkDestroyRenderPass(owner.device, render_pass, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        owner.api->vkDestroyCommandPool(owner.device, pool, nullptr);  // frees the command buffer with it
    }
}

// ---- VulkanDevice ----

VulkanDevice::VulkanDevice() : device_id_(next_device_id()) {}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        // Work still queued may read any object the device holds, so wait for every queue before anything is released.
        static_cast<void>(api_.vkDeviceWaitIdle(device_));
        collect_completed();
        retained_.clear();
        for (auto& waits : pending_waits_) {
            waits.clear();
        }
        // Sets go before their pools and layouts; a set frees itself from its pool, and a pool outlives the sets that hold it.
        sets_.clear();
        pipelines_.clear();
        acceleration_structures_.clear();
        ray_pipelines_.clear();
        layouts_.clear();
        descriptor_pools_.clear();
        buffers_.clear();
        textures_.clear();
        fences_.clear();
        query_pools_.clear();
        for (VkSemaphore timeline : timelines_) {
            if (timeline != VK_NULL_HANDLE) {
                api_.vkDestroySemaphore(device_, timeline, nullptr);
            }
        }
        api_.vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
        api_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        api_.vkDestroyInstance(instance_, nullptr);
    }
}

RhiStatus VulkanDevice::initialize(const DeviceDesc& desc) {
    debug_layers_ = desc.debug_layers;
    if (RhiStatus status = library_.open(); !status.ok()) {
        return status;
    }
    if (RhiStatus status = load_global_functions(api_, library_.get_instance_proc_addr()); !status.ok()) {
        return status;
    }
    std::uint32_t loader_version = 0;
    if (api_.vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS || loader_version < VK_API_VERSION_1_2) {
        return fail(RhiErrorCode::Unsupported, "create_device: the Vulkan loader does not offer Vulkan 1.2");
    }

    // Instance: the surface extensions for the platform, plus the validation layer and its messenger when debug layers are requested.
    const std::vector<std::string> available_extensions = instance_extension_names(api_);
    std::vector<const char*> instance_extensions;
    if (contains(available_extensions, kSurfaceExtension)) {
        instance_extensions.push_back(kSurfaceExtension);
    }
    if (contains(available_extensions, kPlatformSurfaceExtension)) {
        instance_extensions.push_back(kPlatformSurfaceExtension);
    }
    std::vector<const char*> instance_layers;
    if (debug_layers_) {
        const std::vector<std::string> layers = layer_names(api_);
        if (!contains(layers, kValidationLayer)) {
            return fail(RhiErrorCode::Unsupported,
                        "create_device: the Khronos validation layer is not installed (install the Vulkan SDK, or put its manifest on VK_LAYER_PATH)");
        }
        if (!contains(available_extensions, kDebugUtilsExtension)) {
            return fail(RhiErrorCode::Unsupported, "create_device: the loader does not offer VK_EXT_debug_utils, which the validation messages need");
        }
        instance_extensions.push_back(kDebugUtilsExtension);
        instance_layers.push_back(kValidationLayer);
    }

    VkDebugUtilsMessengerCreateInfoEXT messenger_info{};

    messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = &VulkanDevice::debug_callback;
    messenger_info.pUserData = this;

    VkApplicationInfo application{};

    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "OpenWorldEngine";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.pEngineName = "OpenWorldEngine";
    application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info{};

    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pNext = debug_layers_ ? &messenger_info : nullptr;  // catches messages from instance creation itself
    instance_info.pApplicationInfo = &application;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(instance_layers.size());
    instance_info.ppEnabledLayerNames = instance_layers.empty() ? nullptr : instance_layers.data();
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.empty() ? nullptr : instance_extensions.data();
    for (const char* name : instance_extensions) {
        instance_extensions_.emplace_back(name);
    }
    if (const VkResult result = api_.vkCreateInstance(&instance_info, nullptr, &instance_); result != VK_SUCCESS) {
        return core::failure(vk_error("create_device: vkCreateInstance", result));
    }
    if (RhiStatus status = load_instance_functions(api_, library_.get_instance_proc_addr(), instance_); !status.ok()) {
        return status;
    }
    if (debug_layers_) {
        if (api_.vkCreateDebugUtilsMessengerEXT == nullptr) {
            return fail(RhiErrorCode::Unsupported, "create_device: the driver does not export vkCreateDebugUtilsMessengerEXT");
        }
        if (const VkResult result = api_.vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &messenger_); result != VK_SUCCESS) {
            return core::failure(vk_error("create_device: vkCreateDebugUtilsMessengerEXT", result));
        }
    }

    // Adapter: hardware first. A software (CPU) device is used only when the caller allows it, and then only as a development aid.
    std::uint32_t device_count = 0;
    if (const VkResult result = api_.vkEnumeratePhysicalDevices(instance_, &device_count, nullptr); result != VK_SUCCESS) {
        return core::failure(vk_error("create_device: vkEnumeratePhysicalDevices", result));
    }
    std::vector<VkPhysicalDevice> physical_devices(device_count);
    if (device_count > 0) {
        if (const VkResult result = api_.vkEnumeratePhysicalDevices(instance_, &device_count, physical_devices.data()); result != VK_SUCCESS) {
            return core::failure(vk_error("create_device: vkEnumeratePhysicalDevices", result));
        }
    }
    std::vector<Candidate> candidates;
    for (VkPhysicalDevice physical : physical_devices) {
        Candidate candidate;
        if (describe(api_, physical, candidate)) {
            candidates.push_back(std::move(candidate));
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& lhs, const Candidate& rhs) { return lhs.rank() < rhs.rank(); });

    const Candidate* chosen = nullptr;
    if (desc.adapter_index >= 0) {
        if (static_cast<std::size_t>(desc.adapter_index) >= candidates.size()) {
            return fail(RhiErrorCode::Unsupported, "create_device: no Vulkan adapter has the requested adapter_index");
        }
        chosen = &candidates[static_cast<std::size_t>(desc.adapter_index)];
        if (chosen->software() && !desc.allow_software_adapter) {
            return fail(RhiErrorCode::Unsupported, "create_device: the requested adapter is a software adapter");
        }
    } else {
        for (const Candidate& candidate : candidates) {
            if (!candidate.software() || desc.allow_software_adapter) {
                chosen = &candidate;
                break;
            }
        }
        if (chosen == nullptr) {
            return fail(RhiErrorCode::Unsupported,
                        "create_device: no hardware Vulkan 1.2 adapter with timeline semaphores and swapchains is available");
        }
    }
    physical_ = chosen->device;
    report_.adapter_name = chosen->properties.deviceName;
    extensions_ = chosen->extensions;
    limits_ = chosen->properties.limits;
    mutable_descriptors_ = chosen->mutable_descriptors;

    // Queues: graphics owns presentation. Compute and copy take their own families when there are any.
    std::uint32_t family_count = 0;
    api_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    api_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
    const std::uint32_t graphics = chosen->graphics_family;
    const std::uint32_t compute = pick_family(families, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT, graphics);
    const std::uint32_t copy = pick_family(families, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, compute);
    families_ = {graphics, compute, copy};

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    for (std::uint32_t family : families_) {
        const bool seen = std::any_of(queue_infos.begin(), queue_infos.end(),
                                      [family](const VkDeviceQueueCreateInfo& info) { return info.queueFamilyIndex == family; });
        if (!seen) {
            VkDeviceQueueCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            info.queueFamilyIndex = family;
            info.queueCount = 1;
            info.pQueuePriorities = &priority;
            queue_infos.push_back(info);
        }
    }

    // Synchronization2 orders every barrier and submission. Mutable descriptors let one binding hold a buffer or an image, which the
    // RHI's shader resource and unordered access bindings need. Both are enabled here, and only the mutable one is optional.
    VkPhysicalDeviceVulkan12Features enabled12{};

    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.timelineSemaphore = VK_TRUE;
    enabled12.drawIndirectCount = chosen->draw_indirect_count ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceSynchronization2Features enabled_sync2{};
    enabled_sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    enabled_sync2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT enabled_mutable{};
    enabled_mutable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
    enabled_mutable.mutableDescriptorType = VK_TRUE;
    enabled12.pNext = &enabled_sync2;
    if (mutable_descriptors_) {
        enabled_sync2.pNext = &enabled_mutable;
    }
    // Mesh shaders (US-038): the extension's task and mesh stages, and the core store feature the task stage's counters need. The extension's
    // feature struct is valid only under VkPhysicalDeviceFeatures2 (not directly under VkDeviceCreateInfo), so the chain starts with that struct
    // and carries the core features in it. The struct joins only when the adapter has the extension.
    VkPhysicalDeviceMeshShaderFeaturesEXT enabled_mesh{};
    enabled_mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    enabled_mesh.taskShader = VK_TRUE;
    enabled_mesh.meshShader = VK_TRUE;
    VkPhysicalDeviceFeatures2 enabled_features{};
    enabled_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabled_features.features.vertexPipelineStoresAndAtomics = chosen->mesh_shaders ? VK_TRUE : VK_FALSE;
    enabled_features.pNext = &enabled12;
    if (chosen->mesh_shaders) {
        enabled_mesh.pNext = &enabled12;
        enabled_features.pNext = &enabled_mesh;
    }

    // Ray tracing (US-042): buffer device addresses for the acceleration structure inputs, and the two KHR feature structs. They join the
    // head of the chain, in front of whatever the chain already carries.
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enabled_ray{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_acceleration{};
    VkPhysicalDeviceRayQueryFeaturesKHR enabled_query{};
    if (chosen->ray_tracing) {
        enabled12.bufferDeviceAddress = VK_TRUE;
        enabled_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        enabled_query.rayQuery = VK_TRUE;
        enabled_query.pNext = enabled_features.pNext;
        enabled_acceleration.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        enabled_acceleration.accelerationStructure = VK_TRUE;
        enabled_acceleration.pNext = &enabled_query;
        enabled_ray.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        enabled_ray.rayTracingPipeline = VK_TRUE;
        enabled_ray.pNext = &enabled_acceleration;
        enabled_features.pNext = &enabled_ray;
    }

    std::vector<const char*> device_extensions{kSwapchainExtension, kSynchronization2Extension};
    if (mutable_descriptors_) {
        device_extensions.push_back(kMutableDescriptorExtension);
    }
    if (chosen->mesh_shaders) {
        device_extensions.push_back(kMeshShaderExtension);
    }
    if (chosen->ray_tracing) {
        device_extensions.push_back(kRayTracingExtension);
        device_extensions.push_back(kAccelerationStructureExtension);
        device_extensions.push_back(kDeferredHostOperationsExtension);
        device_extensions.push_back(kRayQueryExtension);
        device_extensions.push_back(kPipelineLibraryExtension);
    }
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &enabled_features;
    device_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    if (const VkResult result = api_.vkCreateDevice(physical_, &device_info, nullptr, &device_); result != VK_SUCCESS) {
        return core::failure(vk_error("create_device: vkCreateDevice", result));
    }
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        api_.vkGetDeviceQueue(device_, families_[slot], 0, &queues_[slot]);
    }
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore.pNext = &type;
        if (const VkResult result = api_.vkCreateSemaphore(device_, &semaphore, nullptr, &timelines_[slot]); result != VK_SUCCESS) {
            return core::failure(vk_error("create_device: vkCreateSemaphore for a queue timeline", result));
        }
    }
    api_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    if (api_.vkQueueSubmit2KHR == nullptr || api_.vkCmdPipelineBarrier2KHR == nullptr) {
        return fail(RhiErrorCode::Unsupported, "create_device: the driver does not export the synchronization2 entry points");
    }
    if (chosen->mesh_shaders && api_.vkCmdDrawMeshTasksEXT == nullptr) {
        return fail(RhiErrorCode::Unsupported, "create_device: the driver does not export vkCmdDrawMeshTasksEXT");
    }
    ray_tracing_ = chosen->ray_tracing;
    if (ray_tracing_) {
        if (api_.vkCreateAccelerationStructureKHR == nullptr || api_.vkDestroyAccelerationStructureKHR == nullptr ||
            api_.vkGetAccelerationStructureBuildSizesKHR == nullptr || api_.vkGetAccelerationStructureDeviceAddressKHR == nullptr ||
            api_.vkCmdBuildAccelerationStructuresKHR == nullptr || api_.vkCreateRayTracingPipelinesKHR == nullptr ||
            api_.vkGetRayTracingShaderGroupHandlesKHR == nullptr || api_.vkCmdTraceRaysKHR == nullptr) {
            return fail(RhiErrorCode::Unsupported, "create_device: the driver does not export the ray tracing entry points");
        }
        // The shader table needs the handle size and alignments, and a scratch buffer needs its offset alignment.
        VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{};
        acceleration_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR pipeline_properties{};
        pipeline_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        pipeline_properties.pNext = &acceleration_properties;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &pipeline_properties;
        api_.vkGetPhysicalDeviceProperties2(physical_, &properties);
        shader_handle_size_ = pipeline_properties.shaderGroupHandleSize;
        shader_handle_alignment_ = pipeline_properties.shaderGroupHandleAlignment;
        shader_base_alignment_ = pipeline_properties.shaderGroupBaseAlignment;
        scratch_alignment_ = acceleration_properties.minAccelerationStructureScratchOffsetAlignment;
    }

    // Capability report. Features are reported from the adapter, as DX12 does; a feature the backend does not yet implement fails
    // its commands with Unsupported and names the story that adds it.
    const VkPhysicalDeviceLimits& limits = chosen->properties.limits;
    const bool timestamps = limits.timestampPeriod > 0.0f && chosen->graphics_timestamp_bits > 0 && limits.timestampComputeAndGraphics == VK_TRUE;
    report_.timestamp_frequency_hz = timestamps ? static_cast<std::uint64_t>(1.0e9 / limits.timestampPeriod) : 0;
    report_.backend = Backend::Vulkan;
    report_.validation_layers = debug_layers_;
    for (Feature feature : kFeatureList) {
        bool hardware = false;
        switch (feature) {
            case Feature::MeshShaders:
                hardware = chosen->mesh_shaders;
                break;
            case Feature::IndirectCount:
                hardware = chosen->draw_indirect_count;
                break;
            case Feature::RayTracing:
                hardware = chosen->ray_tracing;
                break;
            case Feature::TimestampQueries:
                hardware = report_.timestamp_frequency_hz > 0;
                break;
            case Feature::None:
                break;
        }
        const std::size_t index = feature_index(feature);
        if (!hardware) {
            report_.features[index] = FeatureState::Unsupported;
        } else {
            report_.features[index] = has_flags(desc.disabled_features, feature) ? FeatureState::DisabledForTest : FeatureState::Supported;
        }
    }
    return core::success();
}

Backend VulkanDevice::backend() const noexcept {
    return Backend::Vulkan;
}

const CapabilityReport& VulkanDevice::capabilities() const noexcept {
    return report_;
}

bool VulkanDevice::has_extension(const char* name) const {
    return contains(extensions_, name);
}

bool VulkanDevice::has_instance_extension(const char* name) const {
    return contains(instance_extensions_, name);
}

BufferObject* VulkanDevice::find_buffer(BufferHandle handle) const {
    return buffers_.find(handle, device_id_);
}

DescriptorSetLayoutObject* VulkanDevice::find_layout(DescriptorSetLayoutHandle handle) const {
    return layouts_.find(handle, device_id_);
}

DescriptorSetObject* VulkanDevice::find_descriptor_set(DescriptorSetHandle handle) const {
    return sets_.find(handle, device_id_);
}

PipelineObject* VulkanDevice::find_pipeline(PipelineHandle handle) const {
    return pipelines_.find(handle, device_id_);
}

TextureObject* VulkanDevice::find_texture(TextureHandle handle) const {
    return textures_.find(handle, device_id_);
}

QueryPoolObject* VulkanDevice::find_query_pool(QueryPoolHandle handle) const {
    return query_pools_.find(handle, device_id_);
}

namespace {

// Finds a memory type for a buffer or image, or nothing when the adapter offers none that fits.
std::optional<std::uint32_t> memory_for(const VkPhysicalDeviceMemoryProperties& properties, const VkMemoryRequirements& requirements,
                                        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    return find_memory_type(properties, requirements.memoryTypeBits, required, preferred);
}

}  // namespace

RhiResult<BufferHandle> VulkanDevice::create_buffer(const BufferDesc& desc) {
    if (desc.size_bytes == 0) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: size must be greater than zero");
    }
    if (desc.usage == BufferUsage::None) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: usage must name at least one use");
    }
    const bool readback = has_flags(desc.usage, BufferUsage::CpuRead);
    const bool upload = has_flags(desc.usage, BufferUsage::CpuWrite);
    if (readback && !has_flags(desc.usage, BufferUsage::CopyDestination)) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: CpuRead needs CopyDestination usage");
    }
    if (upload && !has_flags(desc.usage, BufferUsage::CopySource)) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: CpuWrite needs CopySource usage");
    }
    if (readback && upload) {
        return fail<BufferHandle>(RhiErrorCode::InvalidDescriptor, "create_buffer: CpuRead and CpuWrite cannot be combined");
    }

    auto object = std::make_shared<BufferObject>();
    object->desc = desc;
    object->owner = ref();
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size_bytes;
    info.usage = buffer_usage(desc.usage) | (ray_tracing_ ? kRayTracingBufferUsage : 0);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (const VkResult result = api_.vkCreateBuffer(device_, &info, nullptr, &object->buffer); result != VK_SUCCESS) {
        return core::failure(vk_error("create_buffer: vkCreateBuffer", result));
    }
    VkMemoryRequirements requirements{};
    api_.vkGetBufferMemoryRequirements(device_, object->buffer, &requirements);
    // Readback and upload memory are host-visible and coherent, so a read after the fence and a write before a submit need no explicit
    // invalidate or flush. Readback prefers cached memory, which makes CPU reads faster.
    const std::optional<std::uint32_t> type =
        readback ? memory_for(memory_, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
        : upload ? memory_for(memory_, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)
                 : memory_for(memory_, requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type) {
        return fail<BufferHandle>(RhiErrorCode::OutOfRange, "create_buffer: no memory type fits the buffer");
    }
    VkMemoryAllocateFlagsInfo address_flags = device_address_flags();
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = ray_tracing_ ? &address_flags : nullptr;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    if (const VkResult result = api_.vkAllocateMemory(device_, &allocation, nullptr, &object->memory); result != VK_SUCCESS) {
        return core::failure(vk_error("create_buffer: vkAllocateMemory", result));
    }
    if (const VkResult result = api_.vkBindBufferMemory(device_, object->buffer, object->memory, 0); result != VK_SUCCESS) {
        return core::failure(vk_error("create_buffer: vkBindBufferMemory", result));
    }
    if (readback || upload) {
        if (const VkResult result = api_.vkMapMemory(device_, object->memory, 0, VK_WHOLE_SIZE, 0, &object->mapped); result != VK_SUCCESS) {
            return core::failure(vk_error("create_buffer: vkMapMemory", result));
        }
    }
    // A readback buffer is written by a copy, and an upload buffer is read by one, so each starts in the state that copy needs.
    if (readback) {
        object->state = ResourceState::CopyDestination;
    } else if (upload) {
        object->state = ResourceState::CopySource;
    }
    return core::success(buffers_.add(std::move(object), device_id_));
}

RhiResult<TextureHandle> VulkanDevice::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: width and height must be greater than zero");
    }
    if (desc.format == Format::Unknown) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: a format is required");
    }
    if (desc.usage == TextureUsage::None) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: usage must name at least one use");
    }
    std::uint32_t max_levels = 1;
    for (std::uint32_t dim = std::max(desc.width, desc.height); dim > 1; dim >>= 1) {
        ++max_levels;
    }
    if (desc.mip_levels == 0 || desc.mip_levels > max_levels) {
        return fail<TextureHandle>(RhiErrorCode::OutOfRange, "create_texture: mip_levels must be between 1 and the full chain");
    }
    if (has_flags(desc.usage, TextureUsage::DepthStencil) && !is_depth_format(desc.format)) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: depth-stencil usage needs a depth format");
    }
    if (has_flags(desc.usage, TextureUsage::RenderTarget) && is_depth_format(desc.format)) {
        return fail<TextureHandle>(RhiErrorCode::InvalidDescriptor, "create_texture: render-target usage needs a color format");
    }

    auto object = std::make_shared<TextureObject>();
    object->desc = desc;
    object->owner = ref();
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = to_vk(desc.format);
    info.extent = VkExtent3D{desc.width, desc.height, 1};
    info.mipLevels = desc.mip_levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = image_usage(desc.usage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (const VkResult result = api_.vkCreateImage(device_, &info, nullptr, &object->image); result != VK_SUCCESS) {
        return core::failure(vk_error("create_texture: vkCreateImage", result));
    }
    VkMemoryRequirements requirements{};
    api_.vkGetImageMemoryRequirements(device_, object->image, &requirements);
    const std::optional<std::uint32_t> type = memory_for(memory_, requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type) {
        return fail<TextureHandle>(RhiErrorCode::OutOfRange, "create_texture: no memory type fits the texture");
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    if (const VkResult result = api_.vkAllocateMemory(device_, &allocation, nullptr, &object->memory); result != VK_SUCCESS) {
        return core::failure(vk_error("create_texture: vkAllocateMemory", result));
    }
    if (const VkResult result = api_.vkBindImageMemory(device_, object->image, object->memory, 0); result != VK_SUCCESS) {
        return core::failure(vk_error("create_texture: vkBindImageMemory", result));
    }
    // A view needs a usage it can be used for. A copy-only texture has none, so it gets no view; the copy commands take the image itself.
    if (image_usage(desc.usage) & ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT & ~VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = object->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format;
        view.subresourceRange.aspectMask = aspect_for(desc.format);
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = desc.mip_levels;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        if (const VkResult result = api_.vkCreateImageView(device_, &view, nullptr, &object->view); result != VK_SUCCESS) {
            return core::failure(vk_error("create_texture: vkCreateImageView", result));
        }
    }
    return core::success(textures_.add(std::move(object), device_id_));
}

// Descriptor layouts map one to one onto VkDescriptorSetLayout. A shader resource or unordered access binding is a mutable descriptor, and
// the write that fills it says whether it holds a buffer or an image.
RhiResult<DescriptorSetLayoutHandle> VulkanDevice::create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) {
    if (desc.bindings.empty()) {
        return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "create_descriptor_set_layout: a layout needs at least one binding");
    }
    for (std::size_t i = 0; i < desc.bindings.size(); ++i) {
        const DescriptorBindingDesc& binding = desc.bindings[i];
        if (binding.count == 0) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "create_descriptor_set_layout: binding count must be at least one");
        }
        if (binding.count > kPoolDescriptors) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::OutOfRange, "create_descriptor_set_layout: a binding is larger than a descriptor pool page");
        }
        if (binding.visibility == ShaderStage::None) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "create_descriptor_set_layout: binding needs a shader stage");
        }
        if (binding.type != DescriptorType::ConstantBuffer && !mutable_descriptors_) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::Unsupported,
                                                   "create_descriptor_set_layout: shader resource and unordered access bindings need VK_EXT_mutable_descriptor_type");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (desc.bindings[j].binding == binding.binding) {
                return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "create_descriptor_set_layout: layout repeats a binding index");
            }
        }
    }

    // The mutable lists are kept in a vector that is not resized, so the pointers in the create info stay valid.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(desc.bindings.size());
    std::vector<std::array<VkDescriptorType, 3>> list_storage(desc.bindings.size());
    std::vector<VkMutableDescriptorTypeListEXT> mutable_lists(desc.bindings.size());
    for (std::size_t i = 0; i < desc.bindings.size(); ++i) {
        const DescriptorBindingDesc& binding = desc.bindings[i];
        VkDescriptorSetLayoutBinding entry{};
        entry.binding = binding.binding;
        entry.descriptorType = descriptor_type(binding.type);
        entry.descriptorCount = binding.count;
        entry.stageFlags = stage_flags(binding.visibility);
        bindings.push_back(entry);
        const MutableTypeList list = mutable_types(binding.type, ray_tracing_);
        for (std::uint32_t k = 0; k < list.count; ++k) {
            list_storage[i][k] = list.types[k];
        }
        mutable_lists[i].descriptorTypeCount = list.count;
        mutable_lists[i].pDescriptorTypes = list.count > 0 ? list_storage[i].data() : nullptr;
    }
    VkMutableDescriptorTypeCreateInfoEXT mutable_info{};
    mutable_info.sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT;
    mutable_info.mutableDescriptorTypeListCount = static_cast<std::uint32_t>(mutable_lists.size());
    mutable_info.pMutableDescriptorTypeLists = mutable_lists.data();

    auto object = std::make_shared<DescriptorSetLayoutObject>();
    object->bindings = desc.bindings;
    object->owner = ref();
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = mutable_descriptors_ ? &mutable_info : nullptr;
    info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    if (const VkResult result = api_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &object->layout); result != VK_SUCCESS) {
        return core::failure(vk_error("create_descriptor_set_layout: vkCreateDescriptorSetLayout", result));
    }
    return core::success(layouts_.add(std::move(object), device_id_));
}

RhiResult<DescriptorSetHandle> VulkanDevice::create_descriptor_set(DescriptorSetLayoutHandle layout_handle) {
    const DescriptorSetLayoutObject* layout = layouts_.find(layout_handle, device_id_);
    if (layout == nullptr) {
        return fail<DescriptorSetHandle>(RhiErrorCode::InvalidHandle, "create_descriptor_set: layout handle is invalid or destroyed");
    }
    auto object = std::make_shared<DescriptorSetObject>();
    object->bindings = layout->bindings;
    object->owner = ref();
    object->counters = counters_;
    std::size_t total = 0;
    for (const DescriptorBindingDesc& binding : object->bindings) {
        object->first_entry.push_back(total);
        total += binding.count;
    }
    object->entries.resize(total);
    DescriptorBudget cost;
    cost.sets = 1;
    for (std::size_t i = 0; i < object->bindings.size(); ++i) {
        const DescriptorBindingDesc& binding = object->bindings[i];
        for (std::uint32_t k = 0; k < binding.count; ++k) {
            object->entries[object->first_entry[i] + k].type = binding.type;
        }
        if (binding.type == DescriptorType::ConstantBuffer) {
            cost.uniform += binding.count;
        } else {
            cost.mutable_descriptors += binding.count;
        }
    }
    if (cost.uniform > kPoolDescriptors || cost.mutable_descriptors > kPoolDescriptors) {
        return fail<DescriptorSetHandle>(RhiErrorCode::OutOfRange, "create_descriptor_set: the layout needs more descriptors than a pool page holds");
    }

    // Allocation takes a page with room. A driver that runs a page out before the budget says so retires that page, and the set moves
    // to a new one, so the caller never sees a pool-fragmentation failure.
    RhiResult<std::shared_ptr<DescriptorPoolObject>> pool = pool_with_room(cost);
    if (!pool.ok()) {
        return core::failure(pool.error());
    }
    object->pool = std::move(pool).value();
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = object->pool->pool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &layout->layout;
    VkResult result = api_.vkAllocateDescriptorSets(device_, &allocation, &object->set);
    if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
        object->pool->retired = true;
        pool = pool_with_room(cost);
        if (!pool.ok()) {
            return core::failure(pool.error());
        }
        object->pool = std::move(pool).value();
        allocation.descriptorPool = object->pool->pool;
        result = api_.vkAllocateDescriptorSets(device_, &allocation, &object->set);
    }
    if (result != VK_SUCCESS) {
        return core::failure(vk_error("create_descriptor_set: vkAllocateDescriptorSets", result));
    }
    object->cost = cost;
    object->pool->used.sets += cost.sets;
    object->pool->used.uniform += cost.uniform;
    object->pool->used.mutable_descriptors += cost.mutable_descriptors;
    counters_->shader_descriptors += cost.uniform + cost.mutable_descriptors;
    return core::success(sets_.add(std::move(object), device_id_));
}

RhiResult<std::shared_ptr<DescriptorPoolObject>> VulkanDevice::pool_with_room(const DescriptorBudget& cost) {
    const auto fits = [&cost](const DescriptorBudget& used) {
        return used.sets + cost.sets <= kPoolSets && used.uniform + cost.uniform <= kPoolDescriptors &&
               used.mutable_descriptors + cost.mutable_descriptors <= kPoolDescriptors;
    };
    for (const std::shared_ptr<DescriptorPoolObject>& page : descriptor_pools_) {
        if (!page->retired && fits(page->used)) {
            return core::success(page);
        }
    }

    auto page = std::make_shared<DescriptorPoolObject>();
    page->owner = ref();
    std::vector<VkDescriptorPoolSize> sizes{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kPoolDescriptors}};
    if (mutable_descriptors_) {
        sizes.push_back(VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_MUTABLE_EXT, kPoolDescriptors});
    }
    // Each pool size names its mutable list, so the list of the mutable size is the union that every layout's list fits in.
    std::vector<VkDescriptorType> mutable_pool_types(kPoolMutableTypes.begin(), kPoolMutableTypes.end());
    if (ray_tracing_) {
        mutable_pool_types.push_back(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
    }
    std::vector<VkMutableDescriptorTypeListEXT> lists(sizes.size());
    lists[0].descriptorTypeCount = 0;
    lists[0].pDescriptorTypes = nullptr;
    if (sizes.size() > 1) {
        lists[1].descriptorTypeCount = static_cast<std::uint32_t>(mutable_pool_types.size());
        lists[1].pDescriptorTypes = mutable_pool_types.data();
    }
    VkMutableDescriptorTypeCreateInfoEXT mutable_info{};
    mutable_info.sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT;
    mutable_info.mutableDescriptorTypeListCount = static_cast<std::uint32_t>(lists.size());
    mutable_info.pMutableDescriptorTypeLists = lists.data();

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.pNext = mutable_descriptors_ ? &mutable_info : nullptr;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;  // sets are freed one at a time, when their work completes
    info.maxSets = kPoolSets;
    info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    if (const VkResult result = api_.vkCreateDescriptorPool(device_, &info, nullptr, &page->pool); result != VK_SUCCESS) {
        return core::failure(vk_error("create_descriptor_set: vkCreateDescriptorPool", result));
    }
    descriptor_pools_.push_back(page);
    return core::success(page);
}

RhiResult<PipelineHandle> VulkanDevice::create_pipeline(const PipelineDesc& desc) {
    if (desc.kind == PipelineKind::Mesh && !report_.enabled(Feature::MeshShaders)) {
        return fail<PipelineHandle>(RhiErrorCode::FeatureDisabled, "create_pipeline: mesh pipelines need the MeshShaders feature");
    }
    if (desc.set_layouts.size() > kMaxDescriptorSetsPerPipeline) {
        return fail<PipelineHandle>(RhiErrorCode::OutOfRange, "create_pipeline: too many descriptor set layouts");
    }
    // Graphics and mesh pipelines are drawn in render passes, so both build a render pass below. A mesh pipeline takes its amplification
    // (task) stage optionally, and it has no vertex input.
    const bool graphics = desc.kind == PipelineKind::Graphics;
    const bool mesh = desc.kind == PipelineKind::Mesh;
    const bool rendering = graphics || mesh;
    if (rendering) {
        if (graphics ? desc.shaders.size() != 2 : (desc.shaders.size() < 2 || desc.shaders.size() > 3)) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, graphics ? "create_pipeline: a graphics pipeline needs a vertex and a pixel shader"
                                                                                   : "create_pipeline: a mesh pipeline needs a mesh and a pixel shader");
        }
        if (mesh && (!desc.vertex_attributes.empty() || !desc.instance_attributes.empty())) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a mesh pipeline has no vertex input");
        }
        for (const ShaderBinary& shader : desc.shaders) {
            const bool allowed = graphics ? (shader.stage == ShaderStage::Vertex || shader.stage == ShaderStage::Pixel)
                                          : (shader.stage == ShaderStage::Mesh || shader.stage == ShaderStage::Pixel ||
                                             shader.stage == ShaderStage::Amplification);
            if (!allowed) {
                return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: the shader stages do not match the pipeline kind");
            }
        }
        if (desc.color_formats.size() > kMaxColorTargets) {
            return fail<PipelineHandle>(RhiErrorCode::OutOfRange, "create_pipeline: too many color targets");
        }
        for (const Format format : desc.color_formats) {
            if (format == Format::Unknown || is_depth_format(format)) {
                return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: color targets need a color format");
            }
        }
        if (desc.depth_format != Format::Unknown && !is_depth_format(desc.depth_format)) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: depth target needs a depth format");
        }
        if (const RhiStatus layout = check_vertex_layout(desc); !layout.ok()) {
            return core::failure(layout.error());
        }
        if (const RhiStatus blend = check_blend_state(desc); !blend.ok()) {
            return core::failure(blend.error());
        }
    } else if (desc.shaders.size() != 1 || desc.shaders[0].stage != ShaderStage::Compute || !desc.color_formats.empty() ||
               desc.depth_format != Format::Unknown || !desc.vertex_attributes.empty() || !desc.instance_attributes.empty()) {
        return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: a compute pipeline takes exactly one compute shader");
    }
    for (const ShaderBinary& shader : desc.shaders) {
        if (shader.entry_point.empty() || shader.bytecode.empty() || shader.bytecode.size() % sizeof(std::uint32_t) != 0) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_pipeline: the shader needs an entry point and SPIR-V words");
        }
    }

    // The capability check runs before any Vulkan object exists. It counts what the compute stage would read through the layouts, and
    // rejects a pipeline the adapter cannot bind, instead of letting the driver fail the pipeline layout later.
    auto object = std::make_shared<PipelineObject>();
    object->kind = desc.kind;
    object->owner = ref();
    object->vertex_stride = desc.vertex_stride;
    object->instance_stride = desc.instance_stride;
    std::vector<VkDescriptorSetLayout> set_layouts;
    std::uint32_t uniform_count = 0;
    std::uint32_t storage_count = 0;
    std::uint32_t image_count = 0;
    for (const DescriptorSetLayoutHandle& handle : desc.set_layouts) {
        const DescriptorSetLayoutObject* layout = layouts_.find(handle, device_id_);
        if (layout == nullptr) {
            return fail<PipelineHandle>(RhiErrorCode::InvalidHandle, "create_pipeline: a set layout handle is invalid or destroyed");
        }
        object->layouts.push_back(layout->bindings);
        set_layouts.push_back(layout->layout);
        for (const DescriptorBindingDesc& binding : layout->bindings) {
            if (!has_flags(binding.visibility, ShaderStage::Compute)) {
                continue;
            }
            switch (binding.type) {
                case DescriptorType::ConstantBuffer:
                    uniform_count += binding.count;
                    break;
                case DescriptorType::ShaderResource:
                    // A mutable binding may be a buffer or an image, so it counts against both limits.
                    storage_count += binding.count;
                    image_count += binding.count;
                    break;
                case DescriptorType::UnorderedAccess:
                    storage_count += binding.count;
                    image_count += binding.count;
                    break;
            }
        }
    }
    if (set_layouts.size() > limits_.maxBoundDescriptorSets) {
        return fail<PipelineHandle>(RhiErrorCode::Unsupported, "create_pipeline: the adapter binds fewer descriptor sets than the pipeline uses");
    }
    if (!rendering && uniform_count > limits_.maxPerStageDescriptorUniformBuffers) {
        return fail<PipelineHandle>(RhiErrorCode::Unsupported, "create_pipeline: the compute stage reads more uniform buffers than the adapter allows");
    }
    if (!rendering && storage_count > limits_.maxPerStageDescriptorStorageBuffers) {
        return fail<PipelineHandle>(RhiErrorCode::Unsupported, "create_pipeline: the compute stage reads more storage buffers than the adapter allows");
    }
    if (!rendering && (image_count > limits_.maxPerStageDescriptorSampledImages || image_count > limits_.maxPerStageDescriptorStorageImages)) {
        return fail<PipelineHandle>(RhiErrorCode::Unsupported, "create_pipeline: the compute stage reads more images than the adapter allows");
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.empty() ? nullptr : set_layouts.data();
    if (const VkResult result = api_.vkCreatePipelineLayout(device_, &layout_info, nullptr, &object->layout); result != VK_SUCCESS) {
        return core::failure(vk_error("create_pipeline: vkCreatePipelineLayout", result));
    }

    // SPIR-V is copied into words, so its alignment does not depend on the byte vector the caller passed.
    std::vector<VkShaderModule> modules;
    std::vector<std::vector<std::uint32_t>> words;
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (const ShaderBinary& shader : desc.shaders) {
        std::vector<std::uint32_t> copy(shader.bytecode.size() / sizeof(std::uint32_t));
        std::memcpy(copy.data(), shader.bytecode.data(), shader.bytecode.size());
        words.push_back(std::move(copy));
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = shader.bytecode.size();
        module_info.pCode = words.back().data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (const VkResult result = api_.vkCreateShaderModule(device_, &module_info, nullptr, &module); result != VK_SUCCESS) {
            for (const VkShaderModule created : modules) {
                api_.vkDestroyShaderModule(device_, created, nullptr);
            }
            return core::failure(vk_error("create_pipeline: vkCreateShaderModule", result));
        }
        modules.push_back(module);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = shader.stage == ShaderStage::Vertex          ? VK_SHADER_STAGE_VERTEX_BIT
                    : shader.stage == ShaderStage::Pixel           ? VK_SHADER_STAGE_FRAGMENT_BIT
                    : shader.stage == ShaderStage::Mesh            ? VK_SHADER_STAGE_MESH_BIT_EXT
                    : shader.stage == ShaderStage::Amplification   ? VK_SHADER_STAGE_TASK_BIT_EXT
                                                                   : VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = shader.entry_point.c_str();
        stages.push_back(stage);
    }
    const auto destroy_modules = [this, &modules] {
        for (const VkShaderModule module : modules) {
            api_.vkDestroyShaderModule(device_, module, nullptr);
        }
    };

    VkResult created = VK_SUCCESS;
    if (!rendering) {
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage = stages.front();
        info.layout = object->layout;
        created = api_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &object->pipeline);
        destroy_modules();
        if (created != VK_SUCCESS) {
            return core::failure(vk_error("create_pipeline: vkCreateComputePipelines", created));
        }
        return core::success(pipelines_.add(std::move(object), device_id_));
    }

    // A graphics pipeline is built against a render pass with the same attachment formats as the passes it is drawn in. Vulkan calls
    // such passes compatible, and begin_render_pass creates its own pass with the same formats, so the pipeline can be used in it.
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_refs;
    for (std::size_t i = 0; i < desc.color_formats.size(); ++i) {
        VkAttachmentDescription attachment{};
        attachment.format = to_vk(desc.color_formats[i]);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs.push_back(VkAttachmentReference{static_cast<std::uint32_t>(attachments.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        attachments.push_back(attachment);
    }
    const bool has_depth = desc.depth_format != Format::Unknown;
    VkAttachmentReference depth_ref{};
    if (has_depth) {
        VkAttachmentDescription attachment{};
        attachment.format = to_vk(desc.depth_format);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref = VkAttachmentReference{static_cast<std::uint32_t>(attachments.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        attachments.push_back(attachment);
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(color_refs.size());
    subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
    // The same dependency as the passes that begin_render_pass creates, so the pipeline's pass matches them exactly.
    const VkSubpassDependency dependency = attachment_dependency(has_depth);
    VkRenderPassCreateInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass_info.pAttachments = attachments.empty() ? nullptr : attachments.data();
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dependency;
    if (const VkResult result = api_.vkCreateRenderPass(device_, &pass_info, nullptr, &object->render_pass); result != VK_SUCCESS) {
        destroy_modules();
        return core::failure(vk_error("create_pipeline: vkCreateRenderPass", result));
    }

    // Fixed state (see PipelineDesc). The viewport and scissor are dynamic, because each pass sets them to its target.
    // Binding 0 is per vertex and binding 1 is per instance (PipelineDesc::instance_attributes). An instance binding advances once per
    // instance, starting at the draw's first_instance, so an indirect draw selects its element with that value.
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> inputs;
    if (!desc.vertex_attributes.empty()) {
        bindings.push_back(VkVertexInputBindingDescription{0, desc.vertex_stride, VK_VERTEX_INPUT_RATE_VERTEX});
        for (const VertexAttribute& attribute : desc.vertex_attributes) {
            inputs.push_back(VkVertexInputAttributeDescription{attribute.location, 0, to_vk(attribute.format), attribute.offset});
        }
    }
    if (!desc.instance_attributes.empty()) {
        bindings.push_back(VkVertexInputBindingDescription{1, desc.instance_stride, VK_VERTEX_INPUT_RATE_INSTANCE});
        for (const VertexAttribute& attribute : desc.instance_attributes) {
            inputs.push_back(VkVertexInputAttributeDescription{attribute.location, 1, to_vk(attribute.format), attribute.offset});
        }
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(inputs.size());
    vertex_input.pVertexAttributeDescriptions = inputs.empty() ? nullptr : inputs.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = desc.cull_mode == CullMode::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // Front faces are counter-clockwise on screen, as PipelineDesc says. The GPU suites check that DX12 and Vulkan agree on which faces
    // are front.
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = has_depth ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = has_depth && desc.depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    std::vector<VkPipelineColorBlendAttachmentState> blends(desc.color_formats.size());
    for (VkPipelineColorBlendAttachmentState& blend : blends) {
        // Same factors as the DX12 pipeline: color = src * src.a + dst * (1 - src.a), alpha = src.a + dst.a * (1 - src.a).
        // Additive (US-051) is color = src + dst and alpha = dst.a, the same factors as the DX12 pipeline.
        blend.blendEnable = (desc.alpha_blend || desc.additive_blend) ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = desc.additive_blend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = desc.additive_blend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = desc.additive_blend ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = desc.additive_blend ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<std::uint32_t>(blends.size());
    blend.pAttachments = blends.empty() ? nullptr : blends.data();
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = object->layout;
    info.renderPass = object->render_pass;
    info.subpass = 0;
    created = api_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &object->pipeline);
    destroy_modules();
    if (created != VK_SUCCESS) {
        return core::failure(vk_error("create_pipeline: vkCreateGraphicsPipelines", created));
    }
    return core::success(pipelines_.add(std::move(object), device_id_));
}

RhiResult<FenceHandle> VulkanDevice::create_fence(std::uint64_t initial_value) {
    auto object = std::make_shared<FenceObject>();
    object->owner = ref();
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initial_value;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    if (const VkResult result = api_.vkCreateSemaphore(device_, &info, nullptr, &object->semaphore); result != VK_SUCCESS) {
        return core::failure(vk_error("create_fence: vkCreateSemaphore", result));
    }
    object->signaled = initial_value;
    return core::success(fences_.add(std::move(object), device_id_));
}

RhiResult<QueryPoolHandle> VulkanDevice::create_query_pool(std::uint32_t count) {
    if (count == 0 || count > kMaxQueryCount) {
        return fail<QueryPoolHandle>(RhiErrorCode::OutOfRange, "create_query_pool: count must be between 1 and kMaxQueryCount");
    }
    if (!report_.enabled(Feature::TimestampQueries)) {
        return fail<QueryPoolHandle>(RhiErrorCode::FeatureDisabled, "create_query_pool: the TimestampQueries feature is not enabled");
    }
    auto object = std::make_shared<QueryPoolObject>();
    object->count = count;
    object->written.resize(count);
    object->owner = ref();
    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    if (const VkResult result = api_.vkCreateQueryPool(device_, &info, nullptr, &object->pool); result != VK_SUCCESS) {
        return core::failure(vk_error("create_query_pool: vkCreateQueryPool", result));
    }
    return core::success(query_pools_.add(std::move(object), device_id_));
}

// ---- Ray tracing (US-042) ----

namespace {

// A scratch buffer is never empty, so an update-only build still has memory to read from.
constexpr std::uint64_t kMinScratchBytes = 256;

// Rounds `value` up to a multiple of `alignment`. An alignment of zero means none.
std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// The size query for one structure: the storage it needs, and the scratch of a build and of an update.
VkAccelerationStructureBuildSizesInfoKHR build_sizes(const Api& api, VkDevice device, const VkAccelerationStructureBuildGeometryInfoKHR& inputs,
                                                     std::uint32_t primitives) {
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    api.vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &inputs, &primitives, &sizes);
    return sizes;
}

}  // namespace

VkAccelerationStructureBuildGeometryInfoKHR acceleration_inputs(const AccelerationStructureObject& object, bool refit) {
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    info.type = object.top_level ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = acceleration_build_flags(object.allow_refit);
    info.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.srcAccelerationStructure = refit ? object.structure : VK_NULL_HANDLE;
    info.dstAccelerationStructure = object.structure;
    info.geometryCount = 1;
    info.pGeometries = &object.geometry;
    return info;
}

VkDeviceAddress VulkanDevice::buffer_address(VkBuffer buffer) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return api_.vkGetBufferDeviceAddress(device_, &info);
}

std::shared_ptr<BufferObject> VulkanDevice::share_buffer(BufferObject* buffer) const {
    return std::static_pointer_cast<BufferObject>(buffer->shared_from_this());
}

AccelerationStructureObject* VulkanDevice::find_acceleration(AccelerationStructureHandle handle) const {
    return acceleration_structures_.find(handle, device_id_);
}

RayTracingPipelineObject* VulkanDevice::find_ray_pipeline(RayTracingPipelineHandle handle) const {
    return ray_pipelines_.find(handle, device_id_);
}

RhiResult<std::shared_ptr<BufferObject>> VulkanDevice::create_internal_buffer(std::uint64_t size_bytes, VkBufferUsageFlags usage,
                                                                              bool host_visible, const char* operation) {
    auto object = std::make_shared<BufferObject>();
    object->desc.size_bytes = size_bytes;
    object->owner = ref();
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size_bytes;
    info.usage = usage | kRayTracingBufferUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (const VkResult result = api_.vkCreateBuffer(device_, &info, nullptr, &object->buffer); result != VK_SUCCESS) {
        return core::failure(vk_error(operation, result));
    }
    VkMemoryRequirements requirements{};
    api_.vkGetBufferMemoryRequirements(device_, object->buffer, &requirements);
    // The shader table and the instance records are written by the CPU and read by the GPU, so they are host-visible. Everything else
    // stays in device-local memory.
    const std::optional<std::uint32_t> type =
        host_visible ? memory_for(memory_, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)
                     : memory_for(memory_, requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type) {
        return fail<std::shared_ptr<BufferObject>>(RhiErrorCode::OutOfRange, std::string(operation) + ": no memory type fits the buffer");
    }
    VkMemoryAllocateFlagsInfo address_flags = device_address_flags();
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &address_flags;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *type;
    if (const VkResult result = api_.vkAllocateMemory(device_, &allocation, nullptr, &object->memory); result != VK_SUCCESS) {
        return core::failure(vk_error(operation, result));
    }
    if (const VkResult result = api_.vkBindBufferMemory(device_, object->buffer, object->memory, 0); result != VK_SUCCESS) {
        return core::failure(vk_error(operation, result));
    }
    if (host_visible) {
        if (const VkResult result = api_.vkMapMemory(device_, object->memory, 0, VK_WHOLE_SIZE, 0, &object->mapped); result != VK_SUCCESS) {
            return core::failure(vk_error(operation, result));
        }
    }
    return core::success(std::move(object));
}

RhiStatus VulkanDevice::allocate_structure(AccelerationStructureObject& object, VkAccelerationStructureTypeKHR type,
                                           const VkAccelerationStructureBuildSizesInfoKHR& sizes, const char* operation) {
    auto storage = create_internal_buffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, operation);
    if (!storage.ok()) {
        return core::failure(storage.error());
    }
    const std::uint64_t scratch_bytes = std::max<std::uint64_t>({sizes.buildScratchSize, sizes.updateScratchSize, kMinScratchBytes});
    auto scratch = create_internal_buffer(scratch_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, operation);
    if (!scratch.ok()) {
        return core::failure(scratch.error());
    }
    VkAccelerationStructureCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    info.buffer = storage.value()->buffer;
    info.size = sizes.accelerationStructureSize;
    info.type = type;
    if (const VkResult result = api_.vkCreateAccelerationStructureKHR(device_, &info, nullptr, &object.structure); result != VK_SUCCESS) {
        return core::failure(vk_error(operation, result));
    }
    VkAccelerationStructureDeviceAddressInfoKHR address{};
    address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    address.accelerationStructure = object.structure;
    object.address = api_.vkGetAccelerationStructureDeviceAddressKHR(device_, &address);
    object.storage = std::move(storage).value();
    object.scratch = std::move(scratch).value();
    return core::success();
}

RhiResult<AccelerationStructureHandle> VulkanDevice::create_bottom_level(const BottomLevelDesc& desc) {
    if (!report_.enabled(Feature::RayTracing)) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::FeatureDisabled, "create_bottom_level: the RayTracing feature is not enabled");
    }
    BufferObject* vertices = find_buffer(desc.vertex_buffer);
    if (vertices == nullptr) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidHandle, "create_bottom_level: vertex buffer handle is invalid or destroyed");
    }
    // The GPU reads geometry as shader input during a build, so both buffers need ShaderResource usage to be in the ShaderRead state.
    if (!has_flags(vertices->desc.usage, BufferUsage::Vertex | BufferUsage::ShaderResource)) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor,
                                                 "create_bottom_level: the vertex buffer needs Vertex and ShaderResource usage");
    }
    // The float3 position must fit inside a vertex, and the stride and offset stay on 4-byte boundaries.
    if (desc.vertex_count == 0 || desc.vertex_stride == 0 || desc.vertex_stride % 4 != 0 || desc.position_offset % 4 != 0 ||
        desc.position_offset + 12 > desc.vertex_stride) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor,
                                                 "create_bottom_level: the vertex layout needs a count, a stride and an offset that are multiples of four, and a float3 position inside the stride");
    }
    const std::uint64_t vertex_bytes = static_cast<std::uint64_t>(desc.vertex_count - 1) * desc.vertex_stride + desc.position_offset + 12;
    if (vertex_bytes > vertices->desc.size_bytes) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::OutOfRange, "create_bottom_level: the vertex count exceeds the vertex buffer");
    }

    BufferObject* indices = nullptr;
    if (desc.index_buffer.valid()) {
        indices = find_buffer(desc.index_buffer);
        if (indices == nullptr) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidHandle, "create_bottom_level: index buffer handle is invalid or destroyed");
        }
        if (!has_flags(indices->desc.usage, BufferUsage::Index | BufferUsage::ShaderResource)) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor,
                                                     "create_bottom_level: the index buffer needs Index and ShaderResource usage");
        }
        if (desc.index_count == 0 || desc.index_count % 3 != 0) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor, "create_bottom_level: the index count must be a positive multiple of three");
        }
        if (static_cast<std::uint64_t>(desc.index_count) * (desc.index_format == IndexFormat::Uint16 ? 2u : 4u) > indices->desc.size_bytes) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::OutOfRange, "create_bottom_level: the index count exceeds the index buffer");
        }
    } else {
        if (desc.index_count != 0) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor, "create_bottom_level: an index count needs an index buffer");
        }
        if (desc.vertex_count % 3 != 0) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor,
                                                     "create_bottom_level: non-indexed geometry needs a vertex count that is a multiple of three");
        }
    }

    auto object = std::make_shared<AccelerationStructureObject>();
    object->allow_refit = desc.allow_refit;
    object->owner = ref();
    object->vertex_buffer = share_buffer(vertices);
    VkAccelerationStructureGeometryKHR& geometry = object->geometry;
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = geometry_flags(desc.opaque);
    VkAccelerationStructureGeometryTrianglesDataKHR& triangles = geometry.geometry.triangles;
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = buffer_address(vertices->buffer) + desc.position_offset;
    triangles.vertexStride = desc.vertex_stride;
    triangles.maxVertex = desc.vertex_count - 1;
    triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    if (indices != nullptr) {
        object->index_buffer = share_buffer(indices);
        triangles.indexType = desc.index_format == IndexFormat::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = buffer_address(indices->buffer);
        object->primitive_count = desc.index_count / 3;
    } else {
        object->primitive_count = desc.vertex_count / 3;
    }

    const VkAccelerationStructureBuildGeometryInfoKHR inputs = acceleration_inputs(*object, false);
    const VkAccelerationStructureBuildSizesInfoKHR sizes = build_sizes(api_, device_, inputs, object->primitive_count);
    if (const RhiStatus allocated = allocate_structure(*object, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes, "create_bottom_level");
        !allocated.ok()) {
        return core::failure(allocated.error());
    }
    return core::success(acceleration_structures_.add(std::move(object), device_id_));
}

RhiResult<AccelerationStructureHandle> VulkanDevice::create_top_level(const TopLevelDesc& desc) {
    if (!report_.enabled(Feature::RayTracing)) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::FeatureDisabled, "create_top_level: the RayTracing feature is not enabled");
    }
    if (desc.max_instances == 0) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor, "create_top_level: max_instances must be at least one");
    }
    auto object = std::make_shared<AccelerationStructureObject>();
    object->top_level = true;
    object->allow_refit = desc.allow_refit;
    object->max_instances = desc.max_instances;
    object->primitive_count = desc.max_instances;
    object->owner = ref();
    auto instances = create_internal_buffer(static_cast<std::uint64_t>(desc.max_instances) * sizeof(VkAccelerationStructureInstanceKHR), 0, true,
                                            "create_top_level: instance records");
    if (!instances.ok()) {
        return core::failure(instances.error());
    }
    object->instances = std::move(instances).value();
    VkAccelerationStructureGeometryKHR& geometry = object->geometry;
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = buffer_address(object->instances->buffer);

    const VkAccelerationStructureBuildGeometryInfoKHR inputs = acceleration_inputs(*object, false);
    const VkAccelerationStructureBuildSizesInfoKHR sizes = build_sizes(api_, device_, inputs, desc.max_instances);
    if (const RhiStatus allocated = allocate_structure(*object, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes, "create_top_level");
        !allocated.ok()) {
        return core::failure(allocated.error());
    }
    return core::success(acceleration_structures_.add(std::move(object), device_id_));
}

RhiStatus VulkanDevice::write_top_level(AccelerationStructureHandle structure, const std::vector<TopLevelInstance>& instances) {
    AccelerationStructureObject* object = find_acceleration(structure);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_top_level: structure handle is invalid or destroyed");
    }
    if (!object->top_level) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_top_level: the structure is bottom-level");
    }
    if (instances.size() > object->max_instances) {
        return fail(RhiErrorCode::OutOfRange, "write_top_level: more instances than the structure was created for");
    }
    // The GPU may still read the instance records of a pending build, so the caller waits for those submissions first.
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        if (object->last_use[slot] != 0 && !timeline_complete(kQueueOrder[slot], object->last_use[slot])) {
            return fail(RhiErrorCode::NotReady, "write_top_level: a submission that uses the structure has not completed");
        }
    }
    // Every instance is checked before anything is written, so a rejected list leaves the previous instances in place.
    std::vector<std::shared_ptr<AccelerationStructureObject>> references;
    references.reserve(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) {
        AccelerationStructureObject* bottom = find_acceleration(instances[i].bottom_level);
        if (bottom == nullptr || bottom->top_level) {
            return fail(RhiErrorCode::InvalidHandle, "write_top_level: instance " + std::to_string(i) + " names no live bottom-level structure");
        }
        references.push_back(std::static_pointer_cast<AccelerationStructureObject>(bottom->shared_from_this()));
    }

    auto* records = static_cast<VkAccelerationStructureInstanceKHR*>(object->instances->mapped);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const TopLevelInstance& instance = instances[i];
        VkAccelerationStructureInstanceKHR record{};
        std::memcpy(record.transform.matrix, instance.transform.data(), sizeof record.transform.matrix);
        record.instanceCustomIndex = instance.instance_id & 0xFFFFFFu;
        record.mask = instance.mask;
        record.instanceShaderBindingTableRecordOffset = 0;
        record.flags = 0;
        record.accelerationStructureReference = references[i]->address;
        records[i] = record;
    }
    object->references = std::move(references);
    object->instance_count = static_cast<std::uint32_t>(instances.size());
    object->written = true;
    return core::success();
}

RhiStatus VulkanDevice::destroy(AccelerationStructureHandle structure) {
    std::shared_ptr<AccelerationStructureObject> object = acceleration_structures_.take(structure, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: acceleration structure handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

RhiResult<RayTracingPipelineHandle> VulkanDevice::create_ray_tracing_pipeline(const RayTracingPipelineDesc& desc) {
    if (!report_.enabled(Feature::RayTracing)) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::FeatureDisabled, "create_ray_tracing_pipeline: the RayTracing feature is not enabled");
    }
    if (desc.set_layouts.size() > kMaxDescriptorSetsPerPipeline) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::OutOfRange, "create_ray_tracing_pipeline: too many descriptor set layouts");
    }
    if (desc.library.bytecode.empty() || desc.raygen_entry.empty() || desc.miss_entry.empty() || desc.closest_hit_entry.empty()) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidDescriptor,
                                              "create_ray_tracing_pipeline: needs a SPIR-V library and ray generation, miss, and closest-hit entries");
    }
    if (desc.max_payload_bytes == 0 || desc.max_payload_bytes % 4 != 0 || desc.max_payload_bytes > 128) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_ray_tracing_pipeline: the payload must be a multiple of four, at most 128 bytes");
    }
    if (desc.max_attribute_bytes < 2 || desc.max_attribute_bytes > 32) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_ray_tracing_pipeline: the attribute size must be 2 to 32 bytes");
    }
    if (desc.max_recursion_depth == 0 || desc.max_recursion_depth > 31) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_ray_tracing_pipeline: the recursion depth must be 1 to 31");
    }

    auto object = std::make_shared<RayTracingPipelineObject>();
    object->owner = ref();
    std::vector<VkDescriptorSetLayout> set_layouts;
    for (const DescriptorSetLayoutHandle& layout : desc.set_layouts) {
        const DescriptorSetLayoutObject* source = find_layout(layout);
        if (source == nullptr) {
            return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidHandle, "create_ray_tracing_pipeline: descriptor set layout handle is invalid or destroyed");
        }
        object->layouts.push_back(source->bindings);
        set_layouts.push_back(source->layout);
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.empty() ? nullptr : set_layouts.data();
    if (const VkResult result = api_.vkCreatePipelineLayout(device_, &layout_info, nullptr, &object->layout); result != VK_SUCCESS) {
        return core::failure(vk_error("create_ray_tracing_pipeline: vkCreatePipelineLayout", result));
    }

    // The library is one SPIR-V module with every entry. Each stage names its entry point, so one module serves the three stages.
    std::vector<std::uint32_t> words(desc.library.bytecode.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), desc.library.bytecode.data(), words.size() * sizeof(std::uint32_t));
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = words.size() * sizeof(std::uint32_t);
    module_info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult result = api_.vkCreateShaderModule(device_, &module_info, nullptr, &module); result != VK_SUCCESS) {
        return core::failure(vk_error("create_ray_tracing_pipeline: vkCreateShaderModule", result));
    }
    const std::array<VkPipelineShaderStageCreateInfo, 3> stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_RAYGEN_BIT_KHR, module,
                                        desc.raygen_entry.c_str(), nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MISS_BIT_KHR, module,
                                        desc.miss_entry.c_str(), nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                                        module, desc.closest_hit_entry.c_str(), nullptr},
    };
    // Group 0 is the ray generation, group 1 the miss, and group 2 the triangle hit group. Each shader table record holds one group.
    const std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR, nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                             VK_SHADER_UNUSED_KHR, nullptr},
        VkRayTracingShaderGroupCreateInfoKHR{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                                             VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, VK_SHADER_UNUSED_KHR, 2,
                                             VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, nullptr},
    };
    VkRayTracingPipelineInterfaceCreateInfoKHR iface{};
    iface.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR;
    iface.maxPipelineRayPayloadSize = desc.max_payload_bytes;
    iface.maxPipelineRayHitAttributeSize = desc.max_attribute_bytes;
    VkRayTracingPipelineCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.groupCount = static_cast<std::uint32_t>(groups.size());
    info.pGroups = groups.data();
    info.maxPipelineRayRecursionDepth = desc.max_recursion_depth;
    info.pLibraryInterface = &iface;
    info.layout = object->layout;
    const VkResult created = api_.vkCreateRayTracingPipelinesKHR(device_, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &object->pipeline);
    api_.vkDestroyShaderModule(device_, module, nullptr);
    if (created != VK_SUCCESS) {
        return core::failure(vk_error("create_ray_tracing_pipeline: vkCreateRayTracingPipelinesKHR", created));
    }

    // The shader table holds the handle of each group, one record per group. Each record is a handle padded to the handle alignment, and
    // each region starts on the base alignment, as the spec requires.
    const std::uint64_t handle_size = shader_handle_size_;
    const std::uint64_t record_stride = align_up(handle_size, shader_handle_alignment_);
    const std::uint64_t miss_offset = align_up(record_stride, shader_base_alignment_);
    const std::uint64_t hit_offset = align_up(miss_offset + record_stride, shader_base_alignment_);
    const std::uint64_t table_bytes = hit_offset + record_stride;
    std::vector<std::uint8_t> handles(groups.size() * handle_size);
    if (const VkResult result = api_.vkGetRayTracingShaderGroupHandlesKHR(device_, object->pipeline, 0, static_cast<std::uint32_t>(groups.size()),
                                                                          handles.size(), handles.data());
        result != VK_SUCCESS) {
        return core::failure(vk_error("create_ray_tracing_pipeline: vkGetRayTracingShaderGroupHandlesKHR", result));
    }
    auto table = create_internal_buffer(table_bytes, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, true, "create_ray_tracing_pipeline: shader table");
    if (!table.ok()) {
        return core::failure(table.error());
    }
    auto* bytes = static_cast<std::uint8_t*>(table.value()->mapped);
    const std::array<std::uint64_t, 3> offsets{0, miss_offset, hit_offset};
    for (std::size_t group = 0; group < groups.size(); ++group) {
        std::memcpy(bytes + offsets[group], handles.data() + group * handle_size, handle_size);
    }
    const VkDeviceAddress base = buffer_address(table.value()->buffer);
    object->raygen = VkStridedDeviceAddressRegionKHR{base, record_stride, record_stride};
    object->miss = VkStridedDeviceAddressRegionKHR{base + miss_offset, record_stride, record_stride};
    object->hit = VkStridedDeviceAddressRegionKHR{base + hit_offset, record_stride, record_stride};
    object->shader_table = std::move(table).value();
    return core::success(ray_pipelines_.add(std::move(object), device_id_));
}

RhiStatus VulkanDevice::destroy(RayTracingPipelineHandle pipeline) {
    std::shared_ptr<RayTracingPipelineObject> object = ray_pipelines_.take(pipeline, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: ray tracing pipeline handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}


RhiResult<std::unique_ptr<CommandList>> VulkanDevice::create_command_list(QueueType queue_type) {
    auto data = std::make_shared<ListData>();
    data->owner = ref();
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family(queue_type);
    if (const VkResult result = api_.vkCreateCommandPool(device_, &pool_info, nullptr, &data->pool); result != VK_SUCCESS) {
        return core::failure(vk_error("create_command_list: vkCreateCommandPool", result));
    }
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = data->pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (const VkResult result = api_.vkAllocateCommandBuffers(device_, &allocation, &data->buffer); result != VK_SUCCESS) {
        return core::failure(vk_error("create_command_list: vkAllocateCommandBuffers", result));
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const VkResult result = api_.vkBeginCommandBuffer(data->buffer, &begin); result != VK_SUCCESS) {
        return core::failure(vk_error("create_command_list: vkBeginCommandBuffer", result));
    }
    std::unique_ptr<CommandList> list = std::make_unique<VulkanCommandList>(*this, queue_type, std::move(data));
    return core::success(std::move(list));
}

namespace {

// Finds the binding a write names and checks the array index. Returns the binding's position, or an error.
RhiResult<std::size_t> writable_binding(const DescriptorSetObject& set, std::uint32_t binding, std::uint32_t array_index, const char* op) {
    for (std::size_t position = 0; position < set.bindings.size(); ++position) {
        if (set.bindings[position].binding != binding) {
            continue;
        }
        if (array_index >= set.bindings[position].count) {
            return fail<std::size_t>(RhiErrorCode::OutOfRange, std::string(op) + ": array index is outside the binding");
        }
        return core::success(position);
    }
    return fail<std::size_t>(RhiErrorCode::InvalidDescriptor, std::string(op) + ": the set has no such binding");
}

}  // namespace

RhiStatus VulkanDevice::write_buffer(DescriptorSetHandle set_handle, std::uint32_t binding, std::uint32_t array_index, BufferHandle buffer) {
    DescriptorSetObject* set = sets_.find(set_handle, device_id_);
    if (set == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_buffer: descriptor set handle is invalid or destroyed");
    }
    const BufferObject* target = buffers_.find(buffer, device_id_);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_buffer: buffer handle is invalid or destroyed");
    }
    RhiResult<std::size_t> position = writable_binding(*set, binding, array_index, "write_buffer");
    if (!position.ok()) {
        return core::failure(position.error());
    }
    const DescriptorType type = set->bindings[position.value()].type;
    const bool usage_matches = type == DescriptorType::ConstantBuffer   ? has_flags(target->desc.usage, BufferUsage::Constant)
                               : type == DescriptorType::ShaderResource ? has_flags(target->desc.usage, BufferUsage::ShaderResource)
                                                                        : has_flags(target->desc.usage, BufferUsage::UnorderedAccess);
    if (!usage_matches) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_buffer: buffer usage does not match the binding type");
    }
    const std::optional<VkDescriptorType> written = written_descriptor_type(type, false);
    VkDescriptorBufferInfo info{};
    info.buffer = target->buffer;
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set->set;
    write.dstBinding = binding;
    write.dstArrayElement = array_index;
    write.descriptorCount = 1;
    write.descriptorType = *written;
    write.pBufferInfo = &info;
    api_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    DescriptorEntry& entry = set->entries[set->first_entry[position.value()] + array_index];
    entry.written = true;
    entry.is_texture = false;
    entry.is_structure = false;
    entry.structure = AccelerationStructureHandle{};
    entry.buffer = buffer;
    return core::success();
}

RhiStatus VulkanDevice::write_acceleration_structure(DescriptorSetHandle set_handle, std::uint32_t binding, std::uint32_t array_index,
                                                    AccelerationStructureHandle structure) {
    DescriptorSetObject* set = sets_.find(set_handle, device_id_);
    if (set == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_acceleration_structure: descriptor set handle is invalid or destroyed");
    }
    AccelerationStructureObject* target = find_acceleration(structure);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_acceleration_structure: structure handle is invalid or destroyed");
    }
    if (!target->top_level) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_acceleration_structure: only a top-level structure is bound to a descriptor");
    }
    RhiResult<std::size_t> position = writable_binding(*set, binding, array_index, "write_acceleration_structure");
    if (!position.ok()) {
        return core::failure(position.error());
    }
    if (set->bindings[position.value()].type != DescriptorType::ShaderResource) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_acceleration_structure: the binding must be a shader resource binding");
    }
    VkWriteDescriptorSetAccelerationStructureKHR written_structure{};
    written_structure.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    written_structure.accelerationStructureCount = 1;
    written_structure.pAccelerationStructures = &target->structure;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = &written_structure;
    write.dstSet = set->set;
    write.dstBinding = binding;
    write.dstArrayElement = array_index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    api_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    DescriptorEntry& entry = set->entries[set->first_entry[position.value()] + array_index];
    entry.written = true;
    entry.is_texture = false;
    entry.is_structure = true;
    entry.structure = structure;
    entry.buffer = BufferHandle{};
    entry.image = TextureHandle{};
    return core::success();
}

RhiStatus VulkanDevice::write_texture(DescriptorSetHandle set_handle, std::uint32_t binding, std::uint32_t array_index, TextureHandle texture) {
    DescriptorSetObject* set = sets_.find(set_handle, device_id_);
    if (set == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_texture: descriptor set handle is invalid or destroyed");
    }
    const TextureObject* target = textures_.find(texture, device_id_);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_texture: texture handle is invalid or destroyed");
    }
    RhiResult<std::size_t> position = writable_binding(*set, binding, array_index, "write_texture");
    if (!position.ok()) {
        return core::failure(position.error());
    }
    const DescriptorType type = set->bindings[position.value()].type;
    if (type == DescriptorType::ConstantBuffer) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_texture: a constant buffer binding takes a buffer");
    }
    const bool usage_matches = type == DescriptorType::ShaderResource ? has_flags(target->desc.usage, TextureUsage::ShaderResource)
                                                                       : has_flags(target->desc.usage, TextureUsage::UnorderedAccess);
    if (!usage_matches) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_texture: texture usage does not match the binding type");
    }
    const std::optional<VkDescriptorType> written = written_descriptor_type(type, true);
    VkDescriptorImageInfo info{};
    info.imageView = target->view;
    info.imageLayout = descriptor_image_layout(type);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set->set;
    write.dstBinding = binding;
    write.dstArrayElement = array_index;
    write.descriptorCount = 1;
    write.descriptorType = *written;
    write.pImageInfo = &info;
    api_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    DescriptorEntry& entry = set->entries[set->first_entry[position.value()] + array_index];
    entry.written = true;
    entry.is_texture = true;
    entry.is_structure = false;
    entry.structure = AccelerationStructureHandle{};
    entry.image = texture;
    return core::success();
}

RhiStatus VulkanDevice::destroy(BufferHandle buffer) {
    std::shared_ptr<BufferObject> object = buffers_.take(buffer, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: buffer handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

RhiStatus VulkanDevice::destroy(TextureHandle texture) {
    const TextureObject* live = textures_.find(texture, device_id_);
    if (live == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: texture handle is invalid or already destroyed");
    }
    if (live->swapchain_owned) {
        return fail(RhiErrorCode::InvalidState, "destroy: the swapchain owns this back buffer; destroy the swapchain instead");
    }
    std::shared_ptr<TextureObject> object = textures_.take(texture, device_id_);
    retire(std::move(object));
    return core::success();
}

RhiStatus VulkanDevice::destroy(DescriptorSetLayoutHandle layout) {
    std::shared_ptr<DescriptorSetLayoutObject> object = layouts_.take(layout, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: descriptor set layout handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

RhiStatus VulkanDevice::destroy(DescriptorSetHandle set) {
    std::shared_ptr<DescriptorSetObject> object = sets_.take(set, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: descriptor set handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

RhiStatus VulkanDevice::destroy(PipelineHandle pipeline) {
    std::shared_ptr<PipelineObject> object = pipelines_.take(pipeline, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: pipeline handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

RhiStatus VulkanDevice::destroy(FenceHandle fence) {
    std::shared_ptr<FenceObject> object = fences_.take(fence, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: fence handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}


RhiStatus VulkanDevice::destroy(QueryPoolHandle pool) {
    std::shared_ptr<QueryPoolObject> object = query_pools_.take(pool, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: query pool handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

// Keeps an object until every submission that used it has completed. An object no pending work uses is released here.
void VulkanDevice::retire(std::shared_ptr<ObjectBase> object) {
    object->destroyed = true;
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        const std::uint64_t value = object->last_use[slot];
        if (value != 0 && !timeline_complete(kQueueOrder[slot], value)) {
            retain(Stamp{kQueueOrder[slot], value}, {object});
        }
    }
}

void VulkanDevice::retain(Stamp stamp, std::vector<std::shared_ptr<void>> objects) {
    retained_.push_back(Retained{stamp, std::move(objects)});
}

void VulkanDevice::collect_completed() {
    retained_.erase(std::remove_if(retained_.begin(), retained_.end(),
                                   [this](const Retained& entry) { return timeline_complete(entry.stamp.queue, entry.stamp.value); }),
                    retained_.end());
}

RhiStatus VulkanDevice::submit(QueueType queue_type, const std::vector<CommandList*>& lists, FenceHandle signal_fence,
                               std::uint64_t signal_value) {
    FenceObject* fence = fences_.find(signal_fence, device_id_);
    if (fence == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "submit: signal fence handle is invalid or destroyed");
    }
    if (signal_value <= fence->signaled) {
        return fail(RhiErrorCode::InvalidState, "submit: the signal value must be greater than the fence's last signaled value");
    }
    // Validate every list before any change, so a rejected submission leaves the lists and the device as they were.
    std::vector<VulkanCommandList*> recorders;
    for (CommandList* list : lists) {
        auto* recorder = dynamic_cast<VulkanCommandList*>(list);
        if (recorder == nullptr || &recorder->device_ != this) {
            return fail(RhiErrorCode::InvalidHandle, "submit: command list belongs to another device");
        }
        if (recorder->state_ != VulkanCommandList::ListState::Closed) {
            return fail(RhiErrorCode::InvalidState, "submit: command list must be closed and not yet submitted");
        }
        if (recorder->queue_ != queue_type) {
            return fail(RhiErrorCode::InvalidState, "submit: command list was recorded for another queue");
        }
        if (std::find(recorders.begin(), recorders.end(), recorder) != recorders.end()) {
            return fail(RhiErrorCode::InvalidState, "submit: the same command list appears twice");
        }
        for (const auto& object : recorder->refs_) {
            if (object->destroyed) {
                return fail(RhiErrorCode::InvalidHandle, "submit: a command list uses an object destroyed after it was recorded");
            }
        }
        recorders.push_back(recorder);
    }
    return submit_native(queue_type, recorders, fence, signal_value);
}

RhiStatus VulkanDevice::submit_native(QueueType queue_type, const std::vector<VulkanCommandList*>& lists, FenceObject* fence,
                                      std::uint64_t fence_value) {
    const std::size_t slot = queue_slot(queue_type);

    // A graphics submission also carries the frame syncs its registered swapchains owe: each acquired image is waited on, and each
    // image's render-finished semaphore is signaled for its present.
    SubmitSync sync;
    std::vector<VulkanSwapchain*> contributed;
    if (queue_type == QueueType::Graphics) {
        for (VulkanSwapchain* swapchain : swapchains_) {
            if (swapchain->add_frame_sync(sync)) {
                contributed.push_back(swapchain);
            }
        }
    }
    // Queue waits recorded since the last submission on this queue run before this submission's work.
    for (const PendingWait& wait : pending_waits_[slot]) {
        sync.waits.push_back(wait.fence->semaphore);
        sync.wait_stages.push_back(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        sync.wait_values.push_back(wait.value);
    }
    std::vector<VkCommandBufferSubmitInfo> buffers;
    buffers.reserve(lists.size());
    for (VulkanCommandList* list : lists) {
        VkCommandBufferSubmitInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        buffer.commandBuffer = list->data_->buffer;
        buffers.push_back(buffer);
    }
    const std::uint64_t timeline_value = timeline_values_[slot] + 1;
    sync.signals.push_back(timelines_[slot]);
    sync.signal_values.push_back(timeline_value);
    if (fence != nullptr) {
        sync.signals.push_back(fence->semaphore);
        sync.signal_values.push_back(fence_value);
    }

    std::vector<VkSemaphoreSubmitInfo> waits;
    waits.reserve(sync.waits.size());
    for (std::size_t i = 0; i < sync.waits.size(); ++i) {
        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = sync.waits[i];
        wait.value = sync.wait_values[i];
        wait.stageMask = sync.wait_stages[i];
        waits.push_back(wait);
    }
    std::vector<VkSemaphoreSubmitInfo> signals;
    signals.reserve(sync.signals.size());
    for (std::size_t i = 0; i < sync.signals.size(); ++i) {
        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = sync.signals[i];
        signal.value = sync.signal_values[i];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(signal);
    }
    VkSubmitInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    info.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
    info.pWaitSemaphoreInfos = waits.empty() ? nullptr : waits.data();
    info.commandBufferInfoCount = static_cast<std::uint32_t>(buffers.size());
    info.pCommandBufferInfos = buffers.empty() ? nullptr : buffers.data();
    info.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
    info.pSignalSemaphoreInfos = signals.empty() ? nullptr : signals.data();
    if (const VkResult result = api_.vkQueueSubmit2KHR(queue_handle(queue_type), 1, &info, VK_NULL_HANDLE); result != VK_SUCCESS) {
        return core::failure(vk_error("submit: vkQueueSubmit2", result));
    }

    // The submission was accepted, so commit its effects.
    timeline_values_[slot] = timeline_value;
    for (VulkanSwapchain* swapchain : contributed) {
        swapchain->frame_sync_submitted();
    }
    if (fence != nullptr) {
        fence->signaled = fence_value;
        fence->last_use[slot] = timeline_value;
    }

    // Every object the lists name is held until this submission's timeline value completes, and so is each list's Vulkan data.
    const Stamp stamp{queue_type, timeline_value};
    std::vector<std::shared_ptr<void>> held;
    if (fence != nullptr) {
        held.push_back(fence->shared_from_this());
    }
    for (PendingWait& wait : pending_waits_[slot]) {
        held.push_back(wait.fence);
    }
    pending_waits_[slot].clear();
    for (VulkanCommandList* list : lists) {
        for (const auto& object : list->refs_) {
            object->last_use[slot] = timeline_value;
            held.push_back(object);
        }
        held.push_back(list->data_);
        for (const auto& [pool, index] : list->timestamps_) {
            pool->written[index] = stamp;
        }
        // The device holds the references now, so the list drops its copies. A destroyed object then waits only for the fence.
        list->refs_.clear();
        list->timestamps_.clear();
        list->state_ = VulkanCommandList::ListState::Submitted;
    }
    retain(stamp, std::move(held));
    collect_completed();
    return core::success();
}

RhiStatus VulkanDevice::submit_frame_syncs(const std::function<void(VkCommandBuffer)>& record) {
    if (!record) {
        return submit_native(QueueType::Graphics, {}, nullptr, 0);
    }
    RhiResult<std::unique_ptr<CommandList>> created = create_command_list(QueueType::Graphics);
    if (!created.ok()) {
        return core::failure(created.error());
    }
    std::unique_ptr<CommandList> list = std::move(created).value();
    auto* recorder = static_cast<VulkanCommandList*>(list.get());
    record(recorder->data_->buffer);
    if (RhiStatus closed = list->close(); !closed.ok()) {
        return closed;
    }
    return submit_native(QueueType::Graphics, {recorder}, nullptr, 0);
}

RhiStatus VulkanDevice::run_immediate(QueueType queue_type, const std::function<void(VkCommandBuffer)>& record) {
    RhiResult<std::unique_ptr<CommandList>> created = create_command_list(queue_type);
    if (!created.ok()) {
        return core::failure(created.error());
    }
    std::unique_ptr<CommandList> list = std::move(created).value();
    auto* recorder = static_cast<VulkanCommandList*>(list.get());
    record(recorder->data_->buffer);
    if (RhiStatus closed = list->close(); !closed.ok()) {
        return closed;
    }
    if (RhiStatus submitted = submit_native(queue_type, {recorder}, nullptr, 0); !submitted.ok()) {
        return submitted;
    }
    const RhiStatus waited = wait_timeline(queue_type, timeline_values_[queue_slot(queue_type)]);
    collect_completed();
    return waited;
}

RhiResult<std::uint64_t> VulkanDevice::fence_value(FenceHandle fence) {
    const FenceObject* object = fences_.find(fence, device_id_);
    if (object == nullptr) {
        return fail<std::uint64_t>(RhiErrorCode::InvalidHandle, "fence_value: fence handle is invalid or destroyed");
    }
    std::uint64_t completed = 0;
    if (const VkResult result = api_.vkGetSemaphoreCounterValue(device_, object->semaphore, &completed); result != VK_SUCCESS) {
        return core::failure(vk_error("fence_value: vkGetSemaphoreCounterValue", result));
    }
    return core::success(completed);
}

RhiStatus VulkanDevice::wait_fence(FenceHandle fence, std::uint64_t value) {
    const FenceObject* object = fences_.find(fence, device_id_);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "wait_fence: fence handle is invalid or destroyed");
    }
    if (value > object->signaled) {
        return fail(RhiErrorCode::InvalidState, "wait_fence: no pending submission reaches the requested fence value");
    }
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &object->semaphore;
    wait.pValues = &value;
    if (const VkResult result = api_.vkWaitSemaphores(device_, &wait, std::numeric_limits<std::uint64_t>::max()); result != VK_SUCCESS) {
        return core::failure(vk_error("wait_fence: vkWaitSemaphores", result));
    }
    collect_completed();
    return core::success();
}

bool VulkanDevice::timeline_complete(QueueType queue_type, std::uint64_t value) const {
    std::uint64_t completed = 0;
    if (api_.vkGetSemaphoreCounterValue(device_, timelines_[queue_slot(queue_type)], &completed) != VK_SUCCESS) {
        return false;
    }
    return completed >= value;
}

RhiStatus VulkanDevice::wait_timeline(QueueType queue_type, std::uint64_t value) {
    if (timeline_complete(queue_type, value)) {
        return core::success();
    }
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    VkSemaphore timeline = timelines_[queue_slot(queue_type)];
    wait.semaphoreCount = 1;
    wait.pSemaphores = &timeline;
    wait.pValues = &value;
    if (const VkResult result = api_.vkWaitSemaphores(device_, &wait, std::numeric_limits<std::uint64_t>::max()); result != VK_SUCCESS) {
        return core::failure(vk_error("wait_timeline: vkWaitSemaphores", result));
    }
    return core::success();
}

RhiStatus VulkanDevice::wait_idle() {
    if (const VkResult result = api_.vkDeviceWaitIdle(device_); result != VK_SUCCESS) {
        return core::failure(vk_error("wait_idle: vkDeviceWaitIdle", result));
    }
    collect_completed();
    return core::success();
}

RhiResult<std::uint64_t> VulkanDevice::read_timestamp(QueryPoolHandle pool, std::uint32_t index) {
    const QueryPoolObject* query = query_pools_.find(pool, device_id_);
    if (query == nullptr) {
        return fail<std::uint64_t>(RhiErrorCode::InvalidHandle, "read_timestamp: query pool handle is invalid or destroyed");
    }
    if (index >= query->count) {
        return fail<std::uint64_t>(RhiErrorCode::OutOfRange, "read_timestamp: index is outside the query pool");
    }
    const std::optional<Stamp>& written = query->written[index];
    if (!written || !timeline_complete(written->queue, written->value)) {
        return fail<std::uint64_t>(RhiErrorCode::NotReady, "read_timestamp: the submission that wrote this timestamp has not completed");
    }
    std::uint64_t value = 0;
    const VkResult result = api_.vkGetQueryPoolResults(device_, query->pool, index, 1, sizeof value, &value, sizeof value,
                                                       VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) {
        return fail<std::uint64_t>(RhiErrorCode::NotReady, "read_timestamp: the timestamp is not available yet");
    }
    if (result != VK_SUCCESS) {
        return core::failure(vk_error("read_timestamp: vkGetQueryPoolResults", result));
    }
    return core::success(value);
}

RhiStatus VulkanDevice::read_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, void* destination) {
    const BufferObject* object = buffers_.find(buffer, device_id_);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "read_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(object->desc.usage, BufferUsage::CpuRead)) {
        return fail(RhiErrorCode::InvalidDescriptor, "read_buffer: the buffer was not created with CpuRead usage");
    }
    if (offset > object->desc.size_bytes || size > object->desc.size_bytes - offset) {
        return fail(RhiErrorCode::OutOfRange, "read_buffer: range is outside the buffer");
    }
    if (size == 0) {
        return core::success();
    }
    if (destination == nullptr) {
        return fail(RhiErrorCode::InvalidDescriptor, "read_buffer: destination is null");
    }
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        if (object->last_use[slot] != 0 && !timeline_complete(kQueueOrder[slot], object->last_use[slot])) {
            return fail(RhiErrorCode::NotReady, "read_buffer: a submission that uses the buffer has not completed");
        }
    }
    std::memcpy(destination, static_cast<const std::uint8_t*>(object->mapped) + offset, static_cast<std::size_t>(size));
    return core::success();
}

// Uploads write a host-visible CpuWrite buffer, which the GPU reads through a copy. That buffer is the staging step, as on DX12.
RhiStatus VulkanDevice::upload_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, const void* source) {
    const BufferObject* object = buffers_.find(buffer, device_id_);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "upload_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(object->desc.usage, BufferUsage::CpuWrite)) {
        return fail(RhiErrorCode::InvalidDescriptor, "upload_buffer: the buffer was not created with CpuWrite usage");
    }
    if (size == 0 || offset > object->desc.size_bytes || size > object->desc.size_bytes - offset) {
        return fail(RhiErrorCode::OutOfRange, "upload_buffer: range is outside the buffer");
    }
    if (source == nullptr) {
        return fail(RhiErrorCode::InvalidDescriptor, "upload_buffer: source is null");
    }
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        if (object->last_use[slot] != 0 && !timeline_complete(kQueueOrder[slot], object->last_use[slot])) {
            return fail(RhiErrorCode::NotReady, "upload_buffer: a submission that uses the buffer has not completed");
        }
    }
    // Upload memory is coherent, so the write is visible to the next submission without a flush.
    std::memcpy(static_cast<std::uint8_t*>(object->mapped) + offset, source, static_cast<std::size_t>(size));
    return core::success();
}

RhiStatus VulkanDevice::queue_wait(QueueType queue, FenceHandle fence, std::uint64_t value) {
    FenceObject* object = fences_.find(fence, device_id_);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "queue_wait: fence handle is invalid or destroyed");
    }
    if (value > object->signaled) {
        return fail(RhiErrorCode::InvalidState, "queue_wait: no submission has signaled that fence value yet");
    }
    // The wait is recorded against the queue. The next submission on that queue carries it, so the GPU orders the work and the CPU
    // does not block.
    pending_waits_[queue_slot(queue)].push_back(PendingWait{std::static_pointer_cast<FenceObject>(object->shared_from_this()), value});
    return core::success();
}

RhiResult<ResourceStats> VulkanDevice::resource_stats() {
    ResourceStats stats;
    stats.buffers = static_cast<std::uint32_t>(buffers_.live());
    stats.textures = static_cast<std::uint32_t>(textures_.live());
    stats.descriptor_sets = static_cast<std::uint32_t>(sets_.live());
    stats.shader_descriptors = counters_->shader_descriptors;
    // A hold is one retained entry: a submission, or a destroyed object that a pending submission still uses.
    stats.retained_holds = static_cast<std::uint32_t>(retained_.size());
    return core::success(stats);
}

RhiResult<std::unique_ptr<Swapchain>> VulkanDevice::create_swapchain(const SwapchainDesc& desc) {
    return VulkanSwapchain::create(*this, desc);
}

RhiResult<std::unique_ptr<Upscaler>> VulkanDevice::create_upscaler(const UpscalerDesc& desc) {
    if (desc.max_render_width == 0 || desc.max_render_height == 0 || desc.display_width == 0 || desc.display_height == 0) {
        return fail<std::unique_ptr<Upscaler>>(RhiErrorCode::OutOfRange, "create_upscaler: every size must be greater than zero");
    }
    if (desc.max_render_width > desc.display_width || desc.max_render_height > desc.display_height) {
        return fail<std::unique_ptr<Upscaler>>(RhiErrorCode::OutOfRange, "create_upscaler: the render size must not exceed the display size");
    }
#if ENGINE_FSR2_AVAILABLE
    auto upscaler = std::make_unique<VulkanUpscaler>(*this, desc);
    if (RhiStatus status = upscaler->initialize(debug_layers_); !status.ok()) {
        return core::failure(status.error());
    }
    return core::success(std::unique_ptr<Upscaler>(std::move(upscaler)));
#else
    return fail<std::unique_ptr<Upscaler>>(RhiErrorCode::Unsupported,
                                           "create_upscaler: FSR 2 is not built; its shader permutation headers are not vendored");
#endif
}

RhiStatus VulkanDevice::check_debug_messages() {
    std::lock_guard<std::mutex> lock(messages_mutex_);
    if (errors_.empty()) {
        return core::success();
    }
    std::string report;
    for (const std::string& error : errors_) {
        report += error;
        report += '\n';
    }
    errors_.clear();
    return fail(RhiErrorCode::InvalidState, "Vulkan validation reported errors:\n" + report);
}

void VulkanDevice::record_message(bool error, const char* text) {
    if (!error) {
        return;
    }
    std::lock_guard<std::mutex> lock(messages_mutex_);
    errors_.emplace_back(text != nullptr ? text : "(no message)");
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                            VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* self = static_cast<VulkanDevice*>(user);
    if (self != nullptr && data != nullptr) {
        self->record_message((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0, data->pMessage);
    }
    return VK_FALSE;
}

void VulkanDevice::register_swapchain(VulkanSwapchain* swapchain) {
    swapchains_.push_back(swapchain);
}

void VulkanDevice::unregister_swapchain(VulkanSwapchain* swapchain) {
    swapchains_.erase(std::remove(swapchains_.begin(), swapchains_.end(), swapchain), swapchains_.end());
}

RhiResult<TextureHandle> VulkanDevice::adopt_back_buffer(VkImage image, const TextureDesc& desc) {
    auto object = std::make_shared<TextureObject>();
    object->desc = desc;
    object->image = image;
    object->state = ResourceState::Present;
    object->swapchain_owned = true;
    object->owner = ref();
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = to_vk(desc.format);
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.baseMipLevel = 0;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.baseArrayLayer = 0;
    view.subresourceRange.layerCount = 1;
    if (const VkResult result = api_.vkCreateImageView(device_, &view, nullptr, &object->view); result != VK_SUCCESS) {
        return core::failure(vk_error("swapchain: vkCreateImageView", result));
    }
    return core::success(textures_.add(std::move(object), device_id_));
}

RhiStatus VulkanDevice::release_back_buffer(TextureHandle handle) {
    std::shared_ptr<TextureObject> object = textures_.take(handle, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "release_back_buffer: handle is invalid or already released");
    }
    retire(std::move(object));
    return core::success();
}

RhiResult<std::unique_ptr<Device>> create_vulkan_device(const DeviceDesc& desc) {
    auto device = std::make_unique<VulkanDevice>();
    if (RhiStatus status = device->initialize(desc); !status.ok()) {
        return core::failure(status.error());
    }
    std::unique_ptr<Device> result = std::move(device);
    return core::success(std::move(result));
}

}  // namespace engine::rhi::vulkan
