#pragma once

#include "engine/rhi/capabilities.hpp"
#include "engine/rhi/command_list.hpp"
#include "engine/rhi/error.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/swapchain.hpp"
#include "engine/rhi/types.hpp"
#include "engine/rhi/upscaler.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace engine::rhi {

// Counts of live handles and of storage kept alive by pending submissions (see Device::resource_stats).
struct ResourceStats {
    std::uint32_t buffers = 0;               // live buffer handles
    std::uint32_t textures = 0;              // live texture handles
    std::uint32_t descriptor_sets = 0;       // live descriptor set handles
    std::uint32_t shader_descriptors = 0;    // shader-visible heap slots held by live descriptor sets
    std::uint32_t retained_holds = 0;        // holds that keep destroyed or in-flight objects alive until their fence completes
};

// Abstract GPU device. Every backend implements this contract, and scene, ECS, and game code only see these types.
//
// Lifetime. destroy_*() invalidates the handle at once, so later use is an error. The storage stays alive until every
// submission that recorded the object has completed, which is when wait_fence() or wait_idle() returns. A resource
// referenced by a command list that is not yet submitted is rejected at submit if it was destroyed in between.
//
// Synchronization. submit() signals a fence when its work completes. A caller that reuses memory or a handle after a
// submission must first wait for that fence value. Fence values are monotonic per fence.
//
// Threads. A device is used from one thread at a time. Command lists are recorded on the thread that owns them and
// submitted by the device owner.
class Device {
public:
    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const CapabilityReport& capabilities() const noexcept = 0;

    virtual RhiResult<BufferHandle> create_buffer(const BufferDesc& desc) = 0;
    virtual RhiResult<TextureHandle> create_texture(const TextureDesc& desc) = 0;
    virtual RhiResult<DescriptorSetLayoutHandle> create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) = 0;
    virtual RhiResult<DescriptorSetHandle> create_descriptor_set(DescriptorSetLayoutHandle layout) = 0;
    virtual RhiResult<PipelineHandle> create_pipeline(const PipelineDesc& desc) = 0;
    virtual RhiResult<FenceHandle> create_fence(std::uint64_t initial_value) = 0;
    virtual RhiResult<QueryPoolHandle> create_query_pool(std::uint32_t count) = 0;
    virtual RhiResult<std::unique_ptr<CommandList>> create_command_list(QueueType queue) = 0;

    // Ray tracing (US-041). These fail with FeatureDisabled when the RayTracing feature is not enabled.
    virtual RhiResult<AccelerationStructureHandle> create_bottom_level(const BottomLevelDesc& desc) = 0;
    virtual RhiResult<AccelerationStructureHandle> create_top_level(const TopLevelDesc& desc) = 0;
    // Replaces the instance list of a top-level structure. The instances are copied now, so the caller may drop its vector. Fails with
    // NotReady while a submission that uses the structure is pending, because the GPU may still read the instance memory.
    virtual RhiStatus write_top_level(AccelerationStructureHandle structure, const std::vector<TopLevelInstance>& instances) = 0;
    virtual RhiResult<RayTracingPipelineHandle> create_ray_tracing_pipeline(const RayTracingPipelineDesc& desc) = 0;

    // Writes one array element of a binding. The buffer or texture must match the binding type and usage.
    virtual RhiStatus write_buffer(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, BufferHandle buffer) = 0;
    virtual RhiStatus write_texture(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, TextureHandle texture) = 0;
    // Binds a top-level structure to a ShaderResource binding. The structure must have been built before a dispatch reads it.
    virtual RhiStatus write_acceleration_structure(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index,
                                                   AccelerationStructureHandle structure) = 0;

    virtual RhiStatus destroy(BufferHandle buffer) = 0;
    virtual RhiStatus destroy(TextureHandle texture) = 0;
    virtual RhiStatus destroy(DescriptorSetLayoutHandle layout) = 0;
    virtual RhiStatus destroy(DescriptorSetHandle set) = 0;
    virtual RhiStatus destroy(PipelineHandle pipeline) = 0;
    virtual RhiStatus destroy(FenceHandle fence) = 0;
    virtual RhiStatus destroy(QueryPoolHandle pool) = 0;
    virtual RhiStatus destroy(AccelerationStructureHandle structure) = 0;
    virtual RhiStatus destroy(RayTracingPipelineHandle pipeline) = 0;

    // Queues the lists for `queue`. Each list must be closed, belong to this device, and not be submitted yet. A rejected
    // submission changes nothing. `signal_value` is written to `signal_fence` when the work completes.
    virtual RhiStatus submit(QueueType queue, const std::vector<CommandList*>& lists, FenceHandle signal_fence,
                             std::uint64_t signal_value) = 0;
    // Last value known to be complete. It does not advance pending work.
    virtual RhiResult<std::uint64_t> fence_value(FenceHandle fence) = 0;
    // Blocks until the fence reaches `value`. Fails when no pending submission can reach it.
    virtual RhiStatus wait_fence(FenceHandle fence, std::uint64_t value) = 0;
    virtual RhiStatus wait_idle() = 0;

    // Returns the timestamp written at `index`. Fails with NotReady until the submission that wrote it has completed.
    virtual RhiResult<std::uint64_t> read_timestamp(QueryPoolHandle pool, std::uint32_t index) = 0;

    // Copies bytes out of a CpuRead buffer. Fails with NotReady while any submission that used the buffer is pending.
    virtual RhiStatus read_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, void* destination) = 0;

    // Copies bytes into a CpuWrite buffer. Fails with NotReady while any submission that used the buffer is pending, so the
    // caller waits on that submission's fence before it rewrites memory the GPU may still read.
    virtual RhiStatus upload_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, const void* source) = 0;

    // Makes work submitted later on `queue` wait on the GPU until `fence` reaches `value`. The GPU orders the work; the CPU does
    // not block. The value must already be submitted, so a wait can never name work that does not exist.
    virtual RhiStatus queue_wait(QueueType queue, FenceHandle fence, std::uint64_t value) = 0;

    // Counts of live handles and of storage that pending submissions keep alive. Tests use it to show that repeated allocation and
    // destruction does not grow the device.
    [[nodiscard]] virtual RhiResult<ResourceStats> resource_stats() = 0;

    // Creates a swapchain for a native window. Fails with Unsupported on a device that has no presentation surface.
    virtual RhiResult<std::unique_ptr<Swapchain>> create_swapchain(const SwapchainDesc& desc) = 0;

    // Creates an FSR 2 upscaler (US-046). Fails with Unsupported on a device that cannot run FSR 2, such as the CPU reference device.
    // The upscaler must be destroyed before this device.
    virtual RhiResult<std::unique_ptr<Upscaler>> create_upscaler(const UpscalerDesc& desc) = 0;

    // Returns the errors that the backend's debug/validation layers recorded since the last call. Ok when there are none,
    // and always ok when no layer is active.
    virtual RhiStatus check_debug_messages() = 0;

protected:
    Device() = default;
};

struct DeviceDesc {
    Backend backend = Backend::Validation;
    Feature disabled_features = Feature::None;  // forced off on top of what the adapter reports; tests use this
    // Turns on the backend's debug/validation layers (DX12: the D3D12 debug layer with GPU-based validation). Their errors
    // are returned by check_debug_messages(). Performance measurements run with this off.
    bool debug_layers = false;
    // Adapter in the backend's preference order (high-performance first). -1 selects the first hardware adapter that
    // supports the required feature level.
    std::int32_t adapter_index = -1;
    // Lets a software adapter (WARP on DX12, a CPU device on Vulkan) be selected. Off by default: the GPU suites run on
    // hardware, and a software adapter's results are not a GPU report. Development runs only.
    bool allow_software_adapter = false;
};

// Creates a device for the requested backend. A backend that is not built returns Unsupported, not a fallback.
[[nodiscard]] RhiResult<std::unique_ptr<Device>> create_device(const DeviceDesc& desc);

}  // namespace engine::rhi
