#pragma once

// DX12 backend internals (US-027). Nothing here is public: the RHI headers in engine/rhi/include stay free of DirectX types, and
// the rest of the engine reaches this backend only through engine::rhi::Device.

#include "dx12/dx12_common.hpp"

#include "engine/rhi/device.hpp"
#include "object_lifetime.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace engine::rhi::dx12 {

inline constexpr std::uint32_t kRtvCapacity = 256;
inline constexpr std::uint32_t kDsvCapacity = 64;
// Shader-visible CBV/SRV/UAV descriptors. Each descriptor set takes one contiguous range of them.
inline constexpr std::uint32_t kShaderHeapCapacity = 16384;

// The queue behind each timeline slot (object_lifetime.hpp), in slot order.
inline constexpr std::array<QueueType, kQueueCount> kQueueOrder{QueueType::Graphics, QueueType::Compute, QueueType::Copy};

// The shared owner of a tracked object. Every device object derives from ObjectBase, which is enable_shared_from_this.
template <typename Object>
std::shared_ptr<Object> share(Object* object) {
    return std::static_pointer_cast<Object>(object->shared_from_this());
}

// Upload memory: the CPU writes it and the GPU reads it. It is created in GENERIC_READ and never transitioned (dx12_device.cpp).
HRESULT create_upload_resource(ID3D12Device* device, std::uint64_t size_bytes, ComPtr<ID3D12Resource>& resource);
// One descriptor table per set, in register space = set index (dx12_pipeline.cpp). Graphics, compute, and ray tracing pipelines share it.
RhiResult<ComPtr<ID3D12RootSignature>> build_root_signature(ID3D12Device* device, const std::vector<std::vector<DescriptorBindingDesc>>& layouts);

struct BufferObject final : ObjectBase {
    BufferDesc desc;
    ComPtr<ID3D12Resource> resource;
    ResourceState state = ResourceState::Common;
    void* mapped = nullptr;  // CpuRead buffers stay mapped for their whole life

    ~BufferObject() override;
};

struct TextureObject final : ObjectBase {
    TextureDesc desc;
    ComPtr<ID3D12Resource> resource;
    ResourceState state = ResourceState::Common;
    bool swapchain_owned = false;  // back buffers are destroyed only through their swapchain
    std::optional<std::uint32_t> rtv_slot;
    std::optional<std::uint32_t> dsv_slot;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
};

struct FenceObject final : ObjectBase {
    ComPtr<ID3D12Fence> fence;
    std::uint64_t signaled = 0;  // highest value any submission will signal; wait_fence cannot wait past it
};

struct QueryPoolObject final : ObjectBase {
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;  // one 64-bit timestamp per index, resolved by each write
    std::uint64_t* values = nullptr;  // mapped readback memory
    std::uint32_t count = 0;
    std::vector<std::optional<Stamp>> written;  // submission that will resolve each index, if one is pending

    ~QueryPoolObject() override;
};

// A bottom-level (geometry) or top-level (instances) acceleration structure (US-041). The result and scratch memory are committed when the
// structure is created, sized from the driver's prebuild info. A bottom-level structure keeps the buffers its geometry reads, and a
// top-level one keeps the bottom-level structures in its instance list, so their memory outlives the structures that name them.
struct AccelerationStructureObject final : ObjectBase {
    bool top_level = false;
    bool allow_refit = false;
    ComPtr<ID3D12Resource> result;   // acceleration structure memory
    ComPtr<ID3D12Resource> scratch;  // build scratch, shared by every build of this structure
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    // Bottom-level only: the geometry descriptor a build reads. Its GPU addresses point into the buffers held below.
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    std::shared_ptr<BufferObject> vertex_buffer;
    std::shared_ptr<BufferObject> index_buffer;  // indexed geometry only
    // Top-level only: upload memory holding max_instances D3D12_RAYTRACING_INSTANCE_DESC records, written by write_top_level.
    ComPtr<ID3D12Resource> instances;
    std::uint32_t max_instances = 0;
    std::uint32_t instance_count = 0;   // instances in the list last written
    bool written = false;               // an instance list was written
    std::uint32_t built_instances = 0;  // instance count of the last build
    std::vector<std::shared_ptr<AccelerationStructureObject>> references;  // bottom-level structures the instance list names
    bool built = false;                 // a build has been recorded for this structure
};

// CPU-only descriptor heap with a free list. Descriptors are read when a command that names them is recorded, so a slot can be
// reused as soon as its object is destroyed.
class DescriptorPool {
public:
    RhiStatus initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity);
    [[nodiscard]] std::optional<std::uint32_t> allocate(D3D12_CPU_DESCRIPTOR_HANDLE& handle);
    void release(std::uint32_t slot);

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE start_{};
    std::uint32_t increment_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t next_ = 0;
    std::vector<std::uint32_t> free_;
};

// Shader-visible CBV/SRV/UAV heap (US-029). Descriptor sets take contiguous ranges of it, first fit, and a released range merges with
// its neighbours. The GPU reads these descriptors when a submission executes, so a range is released only after the set that
// owns it is destroyed and no submission still names that set.
class ShaderDescriptorHeap {
public:
    RhiStatus initialize(ID3D12Device* device, std::uint32_t capacity);
    [[nodiscard]] std::optional<std::uint32_t> allocate(std::uint32_t count);
    void release(std::uint32_t first, std::uint32_t count);

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t slot) const;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu(std::uint32_t slot) const;
    [[nodiscard]] ID3D12DescriptorHeap* native() const noexcept { return heap_.Get(); }
    [[nodiscard]] std::uint32_t in_use() const noexcept { return in_use_; }
    [[nodiscard]] std::uint32_t increment() const noexcept { return increment_; }

private:
    struct Range {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{};
    std::uint32_t increment_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t in_use_ = 0;
    std::vector<Range> free_;  // sorted by first slot
};

// One array element of a descriptor set. Exactly one of buffer/texture is set once the element is written.
struct DescriptorElement {
    DescriptorType type = DescriptorType::ConstantBuffer;
    std::shared_ptr<BufferObject> buffer;
    std::shared_ptr<TextureObject> texture;
    std::shared_ptr<AccelerationStructureObject> acceleration;  // ShaderResource only: a top-level structure (US-041)
};

struct DescriptorSetLayoutObject {
    std::vector<DescriptorBindingDesc> bindings;
    std::vector<std::uint32_t> first_element;  // per binding: index of its first array element in a set
    std::uint32_t element_count = 0;
};

// A descriptor set owns a range of the shader-visible heap. The range goes back to the heap when the set is destroyed and every
// submission that named it has completed, which is when the last reference to this object is dropped.
struct DescriptorSetObject final : ObjectBase {
    std::vector<DescriptorBindingDesc> bindings;
    std::vector<std::uint32_t> first_element;
    std::vector<DescriptorElement> elements;
    std::shared_ptr<ShaderDescriptorHeap> heap;
    std::uint32_t first_slot = 0;

    ~DescriptorSetObject() override;
};

// A compute pipeline (US-029). The root signature has one descriptor table per set; table order follows the layout's binding order,
// so the heap ranges of a set line up with the table.
struct PipelineObject final : ObjectBase {
    PipelineKind kind = PipelineKind::Compute;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> state;
    std::vector<std::vector<DescriptorBindingDesc>> layouts;
    std::uint32_t vertex_stride = 0;    // graphics pipelines: bytes per vertex in slot 0
    std::uint32_t instance_stride = 0;  // graphics pipelines: bytes per instance in slot 1 (zero when there is none)
};

// A ray tracing state object (US-041) and its shader table. The table holds the ray generation, miss, and hit group records, one
// kShaderRecordStride slot each, in upload memory written once when the pipeline is created.
struct RayTracingPipelineObject final : ObjectBase {
    ComPtr<ID3D12RootSignature> root_signature;  // the global root signature; compute-style descriptor tables
    ComPtr<ID3D12StateObject> state;
    std::vector<std::vector<DescriptorBindingDesc>> layouts;
    ComPtr<ID3D12Resource> shader_table;
    D3D12_GPU_VIRTUAL_ADDRESS table_address = 0;
};

class Dx12Device final : public Device {
public:
    Dx12Device();
    ~Dx12Device() override;

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

    // ---- Internal API for Dx12CommandList and Dx12Swapchain ----

    [[nodiscard]] std::uint32_t id() const noexcept { return device_id_; }
    [[nodiscard]] ID3D12CommandQueue* queue(QueueType queue) const noexcept;
    [[nodiscard]] ID3D12Device* native() const noexcept { return device_.Get(); }
    [[nodiscard]] IDXGIFactory6* factory() const noexcept { return factory_.Get(); }
    [[nodiscard]] bool tearing_supported() const noexcept { return tearing_supported_; }
    [[nodiscard]] bool debug_layers() const noexcept { return debug_layers_; }

    BufferObject* find_buffer(BufferHandle handle) const;
    TextureObject* find_texture(TextureHandle handle) const;
    QueryPoolObject* find_query_pool(QueryPoolHandle handle) const;
    DescriptorSetLayoutObject* find_layout(DescriptorSetLayoutHandle handle) const;
    DescriptorSetObject* find_set(DescriptorSetHandle handle) const;
    PipelineObject* find_pipeline(PipelineHandle handle) const;
    AccelerationStructureObject* find_acceleration(AccelerationStructureHandle handle) const;
    RayTracingPipelineObject* find_ray_pipeline(RayTracingPipelineHandle handle) const;
    [[nodiscard]] ShaderDescriptorHeap& shader_heap() const noexcept { return *shader_heap_; }
    // The DrawIndexedInstanced command signature that ExecuteIndirect uses (US-037). Made on first use and kept for the device's life.
    [[nodiscard]] RhiResult<ID3D12CommandSignature*> draw_indexed_signature();

    // Signals the queue's timeline after the work already queued on it. Returns the new value.
    [[nodiscard]] RhiResult<std::uint64_t> signal_timeline(QueueType queue);
    [[nodiscard]] bool timeline_complete(QueueType queue, std::uint64_t value) const;
    [[nodiscard]] RhiStatus wait_timeline(QueueType queue, std::uint64_t value);

    // Descriptor sets and pipelines are created with their device methods in dx12_descriptors.cpp and dx12_pipeline.cpp.

    // Swapchain back buffers are device textures that the swapchain creates and destroys.
    [[nodiscard]] RhiResult<TextureHandle> adopt_back_buffer(ComPtr<ID3D12Resource> resource, const TextureDesc& desc);
    [[nodiscard]] RhiStatus release_back_buffer(TextureHandle handle);

private:
    void retain(Stamp stamp, std::vector<std::shared_ptr<void>> objects);
    void release_descriptors(TextureObject& texture);
    void retire(std::shared_ptr<ObjectBase> object);
    void collect_completed();
    [[nodiscard]] RhiStatus wait_for_all_queues();

    std::uint32_t device_id_ = 0;
    CapabilityReport report_;
    bool debug_layers_ = false;
    bool tearing_supported_ = false;

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandSignature> draw_indexed_signature_;
    ComPtr<ID3D12Device5> device5_;  // DXR entry points; set only when the device reports ray tracing
    ComPtr<ID3D12InfoQueue> info_queue_;
    std::array<ComPtr<ID3D12CommandQueue>, kQueueCount> queues_;
    std::array<ComPtr<ID3D12Fence>, kQueueCount> timelines_;
    std::array<std::uint64_t, kQueueCount> timeline_values_{};
    HANDLE wait_event_ = nullptr;

    DescriptorPool rtv_pool_;
    DescriptorPool dsv_pool_;
    std::shared_ptr<ShaderDescriptorHeap> shader_heap_;

    Table<BufferTag, BufferObject> buffers_;
    Table<TextureTag, TextureObject> textures_;
    Table<FenceTag, FenceObject> fences_;
    Table<QueryPoolTag, QueryPoolObject> query_pools_;
    Table<DescriptorSetLayoutTag, DescriptorSetLayoutObject> layouts_;
    Table<DescriptorSetTag, DescriptorSetObject> sets_;
    Table<PipelineTag, PipelineObject> pipelines_;
    Table<AccelerationStructureTag, AccelerationStructureObject> acceleration_structures_;
    Table<RayTracingPipelineTag, RayTracingPipelineObject> ray_pipelines_;

    std::vector<Retained> retained_;
};

// Creates a DX12 device on the adapter that DeviceDesc selects. Fails with Unsupported when no hardware adapter reports D3D12.
[[nodiscard]] RhiResult<std::unique_ptr<Device>> create_dx12_device(const DeviceDesc& desc);

}  // namespace engine::rhi::dx12
