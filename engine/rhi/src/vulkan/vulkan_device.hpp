#pragma once

// Vulkan backend internals (US-028). Nothing here is public: the RHI headers in engine/rhi/include stay free of Vulkan types, and the
// rest of the engine reaches this backend only through engine::rhi::Device.

#include "vulkan/vulkan_api.hpp"

#include "engine/rhi/device.hpp"
#include "object_lifetime.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace engine::rhi::vulkan {

class VulkanCommandList;
class VulkanSwapchain;

// Extra waits and signals that a graphics submission carries for its registered swapchains. Timeline entries use the values in
// the parallel arrays; binary semaphores take 0.
struct SubmitSync {
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags2> wait_stages;
    std::vector<std::uint64_t> wait_values;
    std::vector<VkSemaphore> signals;
    std::vector<std::uint64_t> signal_values;
};

// The entry points and device that an object needs to release its Vulkan handles. Objects are destroyed only while their device is
// alive: the device waits for its queues, then clears its tables, before it destroys the VkDevice.
struct DeviceRef {
    const Api* api = nullptr;
    VkDevice device = VK_NULL_HANDLE;
};

struct BufferObject final : ObjectBase {
    BufferDesc desc;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ResourceState state = ResourceState::Common;
    void* mapped = nullptr;  // CpuRead buffers stay mapped for their whole life
    DeviceRef owner;

    ~BufferObject() override;
};

struct TextureObject final : ObjectBase {
    TextureDesc desc;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;  // VK_NULL_HANDLE for a swapchain image, which the swapchain owns
    ResourceState state = ResourceState::Common;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // the layout the GPU holds; barriers move it along with `state`
    bool swapchain_owned = false;                       // back buffers are destroyed only through their swapchain
    DeviceRef owner;

    ~TextureObject() override;
};

struct FenceObject final : ObjectBase {
    VkSemaphore semaphore = VK_NULL_HANDLE;  // timeline semaphore
    std::uint64_t signaled = 0;              // highest value any submission will signal; wait_fence cannot wait past it
    DeviceRef owner;

    ~FenceObject() override;
};

// Storage that live descriptor sets hold, counted for resource_stats(). A set releases its count when its storage is freed, which is
// after its last submission completes, so the count follows the same rule as the storage.
struct ResourceCounters {
    std::uint32_t shader_descriptors = 0;
};

struct DescriptorSetLayoutObject final : ObjectBase {
    std::vector<DescriptorBindingDesc> bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    DeviceRef owner;

    ~DescriptorSetLayoutObject() override;
};

// The descriptors one set takes from its pool, and the budget a pool page can hold.
struct DescriptorBudget {
    std::uint32_t sets = 0;
    std::uint32_t uniform = 0;  // constant buffer descriptors
    std::uint32_t mutable_descriptors = 0;  // shader resource and unordered access descriptors
};

// A pool page. Sets are freed individually, so the page is reused once its sets are gone. Sets hold their page, so it outlives them.
// A page the driver reports as out of memory is retired: it takes no new sets, and its live sets free themselves as usual.
struct DescriptorPoolObject {
    DeviceRef owner;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    DescriptorBudget used;
    bool retired = false;

    ~DescriptorPoolObject();
};

// One array element of a set's binding. The resource is named by handle, so a destroyed resource is seen as invalid at dispatch.
struct DescriptorEntry {
    DescriptorType type = DescriptorType::ConstantBuffer;
    bool written = false;
    bool is_texture = false;
    bool is_structure = false;  // a top-level acceleration structure (US-042)
    BufferHandle buffer;
    TextureHandle image;
    AccelerationStructureHandle structure;
};

struct DescriptorSetObject final : ObjectBase {
    std::vector<DescriptorBindingDesc> bindings;
    std::vector<std::size_t> first_entry;  // index into `entries` of each binding's first element
    std::vector<DescriptorEntry> entries;
    VkDescriptorSet set = VK_NULL_HANDLE;
    DescriptorBudget cost;
    std::shared_ptr<DescriptorPoolObject> pool;
    std::shared_ptr<ResourceCounters> counters;
    DeviceRef owner;

    ~DescriptorSetObject() override;
};

struct PipelineObject final : ObjectBase {
    PipelineKind kind = PipelineKind::Compute;
    std::vector<std::vector<DescriptorBindingDesc>> layouts;  // the bindings of each set layout, in set-index order
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;  // graphics pipelines: the pass whose attachment formats the pipeline matches
    std::uint32_t vertex_stride = 0;            // graphics pipelines: bytes per vertex in slot 0
    std::uint32_t instance_stride = 0;          // graphics pipelines: bytes per instance in slot 1 (zero when there is none)
    DeviceRef owner;

    ~PipelineObject() override;
};

// A bottom-level or top-level acceleration structure (US-042). The structure's memory, its scratch, and the geometry or instance inputs it
// reads are owned here, so they live as long as the structure's last submission. Vertex and index buffers are kept by name too, so a
// destroyed buffer is seen at build time, as on DX12.
struct AccelerationStructureObject final : ObjectBase {
    bool top_level = false;
    bool allow_refit = false;
    bool built = false;
    VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    std::shared_ptr<BufferObject> storage;   // the structure's memory
    std::shared_ptr<BufferObject> scratch;   // build scratch, reused by every build of this structure
    // Bottom-level: the geometry description and the primitive count of one build.
    std::shared_ptr<BufferObject> vertex_buffer;
    std::shared_ptr<BufferObject> index_buffer;
    VkAccelerationStructureGeometryKHR geometry{};
    std::uint32_t primitive_count = 0;
    // Top-level: the instance records (host-visible), the bottom-level structures they name, and the counts of the last write and build.
    std::shared_ptr<BufferObject> instances;
    std::vector<std::shared_ptr<AccelerationStructureObject>> references;
    std::uint32_t max_instances = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t built_instances = 0;
    bool written = false;
    DeviceRef owner;

    ~AccelerationStructureObject() override;
};

// The build inputs of a structure: its geometry, the type, the flags, and a Build or an Update of the structure in place (refit).
[[nodiscard]] VkAccelerationStructureBuildGeometryInfoKHR acceleration_inputs(const AccelerationStructureObject& object, bool refit);

// A ray tracing pipeline (US-042): the layout and pipeline, and the shader binding table with the three records a trace needs. The table
// is written once, when the pipeline is created, and each region's address is fixed for the pipeline's life.
struct RayTracingPipelineObject final : ObjectBase {
    std::vector<std::vector<DescriptorBindingDesc>> layouts;  // the bindings of each set layout, in set-index order
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::shared_ptr<BufferObject> shader_table;
    VkStridedDeviceAddressRegionKHR raygen{};
    VkStridedDeviceAddressRegionKHR miss{};
    VkStridedDeviceAddressRegionKHR hit{};
    DeviceRef owner;

    ~RayTracingPipelineObject() override;
};

struct QueryPoolObject final : ObjectBase {
    VkQueryPool pool = VK_NULL_HANDLE;
    std::uint32_t count = 0;
    std::vector<std::optional<Stamp>> written;  // submission that will resolve each index, if one is pending
    DeviceRef owner;

    ~QueryPoolObject() override;
};

// The Vulkan objects one command list owns. Submission moves them into the device's retained set, so they outlive the list that
// recorded them. Render passes and framebuffers are created per begin_render_pass and freed with the list.
struct ListData {
    DeviceRef owner;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    std::vector<VkRenderPass> render_passes;
    std::vector<VkFramebuffer> framebuffers;

    ~ListData();
};

class VulkanDevice final : public Device {
public:
    VulkanDevice();
    ~VulkanDevice() override;

    [[nodiscard]] RhiStatus initialize(const DeviceDesc& desc);

    [[nodiscard]] Backend backend() const noexcept override;
    [[nodiscard]] const CapabilityReport& capabilities() const noexcept override;

    RhiResult<BufferHandle> create_buffer(const BufferDesc& desc) override;
    RhiResult<TextureHandle> create_texture(const TextureDesc& desc) override;
    RhiResult<DescriptorSetLayoutHandle> create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) override;
    RhiResult<DescriptorSetHandle> create_descriptor_set(DescriptorSetLayoutHandle layout) override;
    RhiResult<PipelineHandle> create_pipeline(const PipelineDesc& desc) override;
    RhiResult<FenceHandle> create_fence(std::uint64_t initial_value) override;
    RhiResult<QueryPoolHandle> create_query_pool(std::uint32_t count) override;
    RhiResult<std::unique_ptr<CommandList>> create_command_list(QueueType queue) override;
    RhiResult<AccelerationStructureHandle> create_bottom_level(const BottomLevelDesc& desc) override;
    RhiResult<AccelerationStructureHandle> create_top_level(const TopLevelDesc& desc) override;
    RhiStatus write_top_level(AccelerationStructureHandle structure, const std::vector<TopLevelInstance>& instances) override;
    RhiResult<RayTracingPipelineHandle> create_ray_tracing_pipeline(const RayTracingPipelineDesc& desc) override;

    RhiStatus write_buffer(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, BufferHandle buffer) override;
    RhiStatus write_texture(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, TextureHandle texture) override;
    RhiStatus write_acceleration_structure(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index,
                                           AccelerationStructureHandle structure) override;

    RhiStatus destroy(BufferHandle buffer) override;
    RhiStatus destroy(TextureHandle texture) override;
    RhiStatus destroy(DescriptorSetLayoutHandle layout) override;
    RhiStatus destroy(DescriptorSetHandle set) override;
    RhiStatus destroy(PipelineHandle pipeline) override;
    RhiStatus destroy(FenceHandle fence) override;
    RhiStatus destroy(QueryPoolHandle pool) override;
    RhiStatus destroy(AccelerationStructureHandle structure) override;
    RhiStatus destroy(RayTracingPipelineHandle pipeline) override;

    RhiStatus submit(QueueType queue, const std::vector<CommandList*>& lists, FenceHandle signal_fence,
                     std::uint64_t signal_value) override;
    RhiResult<std::uint64_t> fence_value(FenceHandle fence) override;
    RhiStatus wait_fence(FenceHandle fence, std::uint64_t value) override;
    RhiStatus wait_idle() override;
    RhiResult<std::uint64_t> read_timestamp(QueryPoolHandle pool, std::uint32_t index) override;
    RhiStatus read_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, void* destination) override;
    RhiStatus upload_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, const void* source) override;
    RhiStatus queue_wait(QueueType queue, FenceHandle fence, std::uint64_t value) override;
    RhiResult<ResourceStats> resource_stats() override;
    RhiResult<std::unique_ptr<Swapchain>> create_swapchain(const SwapchainDesc& desc) override;
    RhiResult<std::unique_ptr<Upscaler>> create_upscaler(const UpscalerDesc& desc) override;
    RhiStatus check_debug_messages() override;

    // ---- Internal API for VulkanCommandList and VulkanSwapchain ----

    [[nodiscard]] std::uint32_t id() const noexcept { return device_id_; }
    [[nodiscard]] const Api& api() const noexcept { return api_; }
    [[nodiscard]] DeviceRef ref() const noexcept { return DeviceRef{&api_, device_}; }
    [[nodiscard]] VkDevice handle() const noexcept { return device_; }
    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const noexcept { return physical_; }
    [[nodiscard]] const VkPhysicalDeviceLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] bool mutable_descriptors() const noexcept { return mutable_descriptors_; }
    [[nodiscard]] VkQueue queue_handle(QueueType queue_type) const noexcept { return queues_[queue_slot(queue_type)]; }
    [[nodiscard]] std::uint32_t queue_family(QueueType queue_type) const noexcept { return families_[queue_slot(queue_type)]; }
    [[nodiscard]] const VkPhysicalDeviceMemoryProperties& memory_properties() const noexcept { return memory_; }
    [[nodiscard]] bool debug_layers() const noexcept { return debug_layers_; }
    // Whether the adapter has ray tracing (US-042). Buffers then carry device addresses and the acceleration structure descriptor type.
    [[nodiscard]] bool ray_tracing() const noexcept { return ray_tracing_; }
    [[nodiscard]] bool has_extension(const char* name) const;
    [[nodiscard]] bool has_instance_extension(const char* name) const;
    [[nodiscard]] VkDeviceAddress buffer_address(VkBuffer buffer) const;
    [[nodiscard]] std::uint32_t shader_handle_size() const noexcept { return shader_handle_size_; }
    [[nodiscard]] std::uint32_t scratch_alignment() const noexcept { return scratch_alignment_; }
    [[nodiscard]] std::shared_ptr<BufferObject> share_buffer(BufferObject* buffer) const;

    BufferObject* find_buffer(BufferHandle handle) const;
    TextureObject* find_texture(TextureHandle handle) const;
    QueryPoolObject* find_query_pool(QueryPoolHandle handle) const;
    DescriptorSetLayoutObject* find_layout(DescriptorSetLayoutHandle handle) const;
    DescriptorSetObject* find_descriptor_set(DescriptorSetHandle handle) const;
    PipelineObject* find_pipeline(PipelineHandle handle) const;
    AccelerationStructureObject* find_acceleration(AccelerationStructureHandle handle) const;
    RayTracingPipelineObject* find_ray_pipeline(RayTracingPipelineHandle handle) const;

    [[nodiscard]] bool timeline_complete(QueueType queue, std::uint64_t value) const;
    [[nodiscard]] RhiStatus wait_timeline(QueueType queue, std::uint64_t value);

    // Swapchain images are device textures that the swapchain creates and destroys. The view is created here.
    [[nodiscard]] RhiResult<TextureHandle> adopt_back_buffer(VkImage image, const TextureDesc& desc);
    [[nodiscard]] RhiStatus release_back_buffer(TextureHandle handle);

    void register_swapchain(VulkanSwapchain* swapchain);
    void unregister_swapchain(VulkanSwapchain* swapchain);
    // Submits to the graphics queue the frame syncs that registered swapchains have pending: an acquired image's wait and its
    // render-finished signal. Present calls this when no list was submitted for the frame, so the image is still ordered. A
    // `record` callback, when given, fills the command buffer that the submission carries, such as a first-use transition.
    [[nodiscard]] RhiStatus submit_frame_syncs(const std::function<void(VkCommandBuffer)>& record = {});
    // Records one command buffer with `record`, submits it on `queue`, and waits for it. For setup work that must finish before the
    // caller continues, such as the first layout transition of a swapchain image.
    [[nodiscard]] RhiStatus run_immediate(QueueType queue, const std::function<void(VkCommandBuffer)>& record);

private:
    RhiStatus submit_native(QueueType queue_type, const std::vector<VulkanCommandList*>& lists, FenceObject* fence,
                            std::uint64_t fence_value);
    // Creates a buffer for the backend's own use (acceleration structure storage, scratch, instance records, shader table). Its memory is
    // host-visible when `host_visible`, and it carries device addresses when the device has ray tracing.
    [[nodiscard]] RhiResult<std::shared_ptr<BufferObject>> create_internal_buffer(std::uint64_t size_bytes, VkBufferUsageFlags usage,
                                                                                  bool host_visible, const char* operation);
    // Allocates the memory and scratch of a new structure from the driver's size estimate, then creates the structure over that memory.
    [[nodiscard]] RhiStatus allocate_structure(AccelerationStructureObject& object, VkAccelerationStructureTypeKHR type,
                                               const VkAccelerationStructureBuildSizesInfoKHR& sizes, const char* operation);
    // Takes a pool page with room for `cost`, or creates one. A page that is full is not used again until a set is freed.
    RhiResult<std::shared_ptr<DescriptorPoolObject>> pool_with_room(const DescriptorBudget& cost);
    void retain(Stamp stamp, std::vector<std::shared_ptr<void>> objects);
    void retire(std::shared_ptr<ObjectBase> object);
    void collect_completed();
    void record_message(bool error, const char* text);
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT types,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    // The loader stays open until the last member is destroyed, after every object that calls into it.
    VulkanLibrary library_;
    Api api_;

    std::uint32_t device_id_ = 0;
    CapabilityReport report_;
    bool debug_layers_ = false;
    std::vector<std::string> extensions_;           // device extensions the device exposes
    std::vector<std::string> instance_extensions_;  // instance extensions this device enabled

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkPhysicalDeviceLimits limits_{};
    bool mutable_descriptors_ = false;  // VK_EXT_mutable_descriptor_type is enabled; shader resource bindings need it
    bool ray_tracing_ = false;          // the adapter has VK_KHR_ray_tracing_pipeline and VK_KHR_acceleration_structure, enabled here (US-042)
    // Shader group and scratch alignments from the driver (US-042). A shader table record is a handle, padded to the handle alignment.
    std::uint32_t shader_handle_size_ = 0;
    std::uint32_t shader_handle_alignment_ = 0;
    std::uint32_t shader_base_alignment_ = 0;
    std::uint32_t scratch_alignment_ = 0;
    std::array<std::uint32_t, kQueueCount> families_{};
    std::array<VkQueue, kQueueCount> queues_{};
    std::array<VkSemaphore, kQueueCount> timelines_{};
    std::array<std::uint64_t, kQueueCount> timeline_values_{};
    std::vector<VulkanSwapchain*> swapchains_;

    Table<BufferTag, BufferObject> buffers_;
    Table<TextureTag, TextureObject> textures_;
    Table<FenceTag, FenceObject> fences_;
    Table<QueryPoolTag, QueryPoolObject> query_pools_;
    Table<DescriptorSetLayoutTag, DescriptorSetLayoutObject> layouts_;
    Table<DescriptorSetTag, DescriptorSetObject> sets_;
    Table<PipelineTag, PipelineObject> pipelines_;
    Table<AccelerationStructureTag, AccelerationStructureObject> acceleration_structures_;
    Table<RayTracingPipelineTag, RayTracingPipelineObject> ray_pipelines_;
    std::vector<std::shared_ptr<DescriptorPoolObject>> descriptor_pools_;
    std::shared_ptr<ResourceCounters> counters_ = std::make_shared<ResourceCounters>();

    // A queue_wait names a fence value that is already submitted. It becomes a wait on the next submission on that queue, and the
    // fence stays alive until that submission is made.
    struct PendingWait {
        std::shared_ptr<FenceObject> fence;
        std::uint64_t value = 0;
    };
    std::array<std::vector<PendingWait>, kQueueCount> pending_waits_;

    std::vector<Retained> retained_;

    // Validation messages arrive on the thread that caused them, so they are collected under a lock.
    mutable std::mutex messages_mutex_;
    std::vector<std::string> errors_;
};

// Creates a Vulkan device on the adapter that DeviceDesc selects. Fails with Unsupported when no loader, no Vulkan 1.2 driver, or no
// hardware adapter (or the requested software adapter, when allowed) is available.
[[nodiscard]] RhiResult<std::unique_ptr<Device>> create_vulkan_device(const DeviceDesc& desc);

}  // namespace engine::rhi::vulkan
