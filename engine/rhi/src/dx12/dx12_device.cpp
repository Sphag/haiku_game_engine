#include "dx12/dx12_device.hpp"

#include "dx12/dx12_command_list.hpp"
#include "dx12/dx12_swapchain.hpp"

#include "engine/core/result.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <utility>

namespace engine::rhi::dx12 {
namespace {

// Device ids start high so a DX12 handle never matches a Validation device's id, even when both devices exist.
std::uint32_t next_device_id() {
    static std::atomic<std::uint32_t> next{0x40000000u};
    return next.fetch_add(1);
}

constexpr std::array<D3D12_COMMAND_LIST_TYPE, kQueueCount> kListTypes{
    D3D12_COMMAND_LIST_TYPE_DIRECT,
    D3D12_COMMAND_LIST_TYPE_COMPUTE,
    D3D12_COMMAND_LIST_TYPE_COPY,
};

}  // namespace

// Upload memory is written by the CPU and read by the GPU, so it stays in GENERIC_READ for its whole life.
HRESULT create_upload_resource(ID3D12Device* device, std::uint64_t size_bytes, ComPtr<ID3D12Resource>& resource) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           IID_PPV_ARGS(&resource));
}

namespace {

// Readback memory: CPU-visible, and only ever in the CopyDestination state.
HRESULT create_readback_resource(ID3D12Device* device, std::uint64_t size_bytes, ComPtr<ID3D12Resource>& resource) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&resource));
}

}  // namespace

BufferObject::~BufferObject() {
    if (mapped != nullptr) {
        resource->Unmap(0, nullptr);
    }
}

QueryPoolObject::~QueryPoolObject() {
    if (values != nullptr) {
        readback->Unmap(0, nullptr);
    }
}

// ---- DescriptorPool ----

RhiStatus DescriptorPool::initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
    if (FAILED(hr)) {
        return core::failure(hresult_error("create_device: CreateDescriptorHeap", hr));
    }
    start_ = heap_->GetCPUDescriptorHandleForHeapStart();
    increment_ = device->GetDescriptorHandleIncrementSize(type);
    capacity_ = capacity;
    return core::success();
}

std::optional<std::uint32_t> DescriptorPool::allocate(D3D12_CPU_DESCRIPTOR_HANDLE& handle) {
    std::uint32_t slot = 0;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else if (next_ < capacity_) {
        slot = next_++;
    } else {
        return std::nullopt;
    }
    handle = start_;
    handle.ptr += static_cast<SIZE_T>(slot) * increment_;
    return slot;
}

void DescriptorPool::release(std::uint32_t slot) {
    free_.push_back(slot);
}

// ---- Dx12Device ----

Dx12Device::Dx12Device() : device_id_(next_device_id()) {}

Dx12Device::~Dx12Device() {
    if (device_) {
        // Work still queued may read any object the device holds, so wait for it before anything is released.
        static_cast<void>(wait_for_all_queues());
        collect_completed();
    }
    if (wait_event_ != nullptr) {
        CloseHandle(wait_event_);
    }
}

RhiStatus Dx12Device::initialize(const DeviceDesc& desc) {
    debug_layers_ = desc.debug_layers;
    UINT factory_flags = 0;
    if (desc.debug_layers) {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            return fail(RhiErrorCode::Unsupported,
                        "create_device: the D3D12 debug layer is not installed (enable the Graphics Tools optional feature)");
        }
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) {
            debug1->SetEnableGPUBasedValidation(TRUE);
        }
        factory_flags = DXGI_CREATE_FACTORY_DEBUG;
    }

    HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        return fail(RhiErrorCode::Unsupported, "create_device: CreateDXGIFactory2 failed with " + hresult_text(hr));
    }

    // Adapters come in high-performance order. Software adapters (WARP) are skipped unless named by index, and then rejected:
    // the reference rule is that the GPU suites run on hardware.
    bool found = false;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        hr = factory_->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            return fail(RhiErrorCode::Unsupported, "create_device: EnumAdapterByGpuPreference failed with " + hresult_text(hr));
        }
        DXGI_ADAPTER_DESC1 candidate_desc{};
        candidate->GetDesc1(&candidate_desc);
        const bool software = (candidate_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (desc.adapter_index >= 0) {
            if (static_cast<std::int32_t>(index) != desc.adapter_index) {
                continue;
            }
            if (software && !desc.allow_software_adapter) {
                return fail(RhiErrorCode::Unsupported, "create_device: the requested adapter is a software adapter");
            }
        } else if (software && !desc.allow_software_adapter) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
            adapter_desc = candidate_desc;
            found = true;
            break;
        }
        if (desc.adapter_index >= 0) {
            return fail(RhiErrorCode::Unsupported, "create_device: the requested adapter does not support feature level 12_0");
        }
    }
    if (!found) {
        return fail(RhiErrorCode::Unsupported, "create_device: no hardware adapter supports Direct3D feature level 12_0");
    }

    report_.backend = Backend::Dx12;
    report_.adapter_name = wide_to_utf8(adapter_desc.Description);
    report_.validation_layers = debug_layers_;
    if (debug_layers_) {
        if (FAILED(device_.As(&info_queue_))) {
            return fail(RhiErrorCode::Unsupported, "create_device: the debug layer did not expose ID3D12InfoQueue");
        }
    }

    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = kListTypes[slot];
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queues_[slot]));
        if (FAILED(hr)) {
            return core::failure(hresult_error("create_device: CreateCommandQueue", hr));
        }
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timelines_[slot]));
        if (FAILED(hr)) {
            return core::failure(hresult_error("create_device: CreateFence for a queue timeline", hr));
        }
    }
    wait_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wait_event_ == nullptr) {
        return fail(RhiErrorCode::Unsupported, "create_device: CreateEventW failed");
    }

    std::uint64_t frequency = 0;
    queues_[queue_slot(QueueType::Graphics)]->GetTimestampFrequency(&frequency);
    report_.timestamp_frequency_hz = frequency;

    if (RhiStatus status = rtv_pool_.initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCapacity); !status.ok()) {
        return status;
    }
    if (RhiStatus status = dsv_pool_.initialize(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCapacity); !status.ok()) {
        return status;
    }
    shader_heap_ = std::make_shared<ShaderDescriptorHeap>();
    if (RhiStatus status = shader_heap_->initialize(device_.Get(), kShaderHeapCapacity); !status.ok()) {
        return status;
    }

    // Hardware support, then the test override. A feature the hardware lacks stays Unsupported even if the test asks for it.
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof options5);
    device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof options7);
    // DXR entry points (ID3D12Device5) come with the tier; a driver that reports the tier without them is treated as lacking ray tracing.
    if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) {
        if (FAILED(device_.As(&device5_))) {
            device5_.Reset();
        }
    }
    const std::array<bool, kFeatureCount> hardware{
        options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1,              // MeshShaders
        true,                                                             // IndirectCount: ExecuteIndirect count is core D3D12
        options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 && device5_.Get() != nullptr,  // RayTracing
        frequency > 0,                                         // TimestampQueries
    };
    for (Feature feature : kFeatureList) {
        const std::size_t index = feature_index(feature);
        if (!hardware[index]) {
            report_.features[index] = FeatureState::Unsupported;
        } else {
            report_.features[index] = has_flags(desc.disabled_features, feature) ? FeatureState::DisabledForTest : FeatureState::Supported;
        }
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory_.As(&factory5))) {
        BOOL allow_tearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof allow_tearing))) {
            tearing_supported_ = allow_tearing != FALSE;
        }
    }
    return core::success();
}

Backend Dx12Device::backend() const noexcept {
    return Backend::Dx12;
}

const CapabilityReport& Dx12Device::capabilities() const noexcept {
    return report_;
}

ID3D12CommandQueue* Dx12Device::queue(QueueType queue) const noexcept {
    return queues_[queue_slot(queue)].Get();
}

BufferObject* Dx12Device::find_buffer(BufferHandle handle) const {
    return buffers_.find(handle, device_id_);
}

TextureObject* Dx12Device::find_texture(TextureHandle handle) const {
    return textures_.find(handle, device_id_);
}

QueryPoolObject* Dx12Device::find_query_pool(QueryPoolHandle handle) const {
    return query_pools_.find(handle, device_id_);
}

RhiResult<BufferHandle> Dx12Device::create_buffer(const BufferDesc& desc) {
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
    if (upload) {
        if (const HRESULT hr = create_upload_resource(device_.Get(), desc.size_bytes, object->resource); FAILED(hr)) {
            return core::failure(hresult_error("create_buffer", hr));
        }
        object->state = ResourceState::CopySource;
        if (const HRESULT hr = object->resource->Map(0, nullptr, &object->mapped); FAILED(hr)) {
            return core::failure(hresult_error("create_buffer: Map", hr));
        }
    } else if (readback) {
        if (const HRESULT hr = create_readback_resource(device_.Get(), desc.size_bytes, object->resource); FAILED(hr)) {
            return core::failure(hresult_error("create_buffer", hr));
        }
        object->state = ResourceState::CopyDestination;
        if (const HRESULT hr = object->resource->Map(0, nullptr, &object->mapped); FAILED(hr)) {
            return core::failure(hresult_error("create_buffer: Map", hr));
        }
    } else {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC resource{};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        // A constant buffer view covers whole 256-byte blocks, so the resource is padded to them.
        const std::uint64_t width = has_flags(desc.usage, BufferUsage::Constant) ? (desc.size_bytes + 255) / 256 * 256 : desc.size_bytes;
        resource.Width = width;
        resource.Height = 1;
        resource.DepthOrArraySize = 1;
        resource.MipLevels = 1;
        resource.Format = DXGI_FORMAT_UNKNOWN;
        resource.SampleDesc.Count = 1;
        resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (has_flags(desc.usage, BufferUsage::UnorderedAccess)) {
            resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        if (const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_COMMON,
                                                                nullptr, IID_PPV_ARGS(&object->resource));
            FAILED(hr)) {
            return core::failure(hresult_error("create_buffer", hr));
        }
    }
    set_name(object->resource.Get(), desc.debug_name);
    return core::success(buffers_.add(std::move(object), device_id_));
}

RhiResult<TextureHandle> Dx12Device::create_texture(const TextureDesc& desc) {
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
    // A depth texture that shaders read is typeless, so the depth view and the color view can name different formats.
    const bool typeless_depth = is_depth_format(desc.format) && has_flags(desc.usage, TextureUsage::ShaderResource);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.width;
    resource.Height = desc.height;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = static_cast<UINT16>(desc.mip_levels);
    resource.Format = typeless_depth ? DXGI_FORMAT_R32_TYPELESS : to_dxgi(desc.format);
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (has_flags(desc.usage, TextureUsage::RenderTarget)) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (has_flags(desc.usage, TextureUsage::DepthStencil)) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    if (has_flags(desc.usage, TextureUsage::UnorderedAccess)) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_COMMON,
                                                            nullptr, IID_PPV_ARGS(&object->resource));
        FAILED(hr)) {
        return core::failure(hresult_error("create_texture", hr));
    }
    set_name(object->resource.Get(), desc.debug_name);

    if (has_flags(desc.usage, TextureUsage::RenderTarget)) {
        const auto slot = rtv_pool_.allocate(object->rtv);
        if (!slot) {
            return fail<TextureHandle>(RhiErrorCode::OutOfRange, "create_texture: no render-target descriptors left");
        }
        object->rtv_slot = *slot;
        device_->CreateRenderTargetView(object->resource.Get(), nullptr, object->rtv);
    }
    if (has_flags(desc.usage, TextureUsage::DepthStencil)) {
        const auto slot = dsv_pool_.allocate(object->dsv);
        if (!slot) {
            return fail<TextureHandle>(RhiErrorCode::OutOfRange, "create_texture: no depth-stencil descriptors left");
        }
        object->dsv_slot = *slot;
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(object->resource.Get(), &view, object->dsv);
    }
    return core::success(textures_.add(std::move(object), device_id_));
}

RhiResult<FenceHandle> Dx12Device::create_fence(std::uint64_t initial_value) {
    auto object = std::make_shared<FenceObject>();
    if (const HRESULT hr = device_->CreateFence(initial_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&object->fence)); FAILED(hr)) {
        return core::failure(hresult_error("create_fence", hr));
    }
    object->signaled = initial_value;
    return core::success(fences_.add(std::move(object), device_id_));
}

RhiResult<QueryPoolHandle> Dx12Device::create_query_pool(std::uint32_t count) {
    if (count == 0 || count > kMaxQueryCount) {
        return fail<QueryPoolHandle>(RhiErrorCode::OutOfRange, "create_query_pool: count must be between 1 and kMaxQueryCount");
    }
    if (!report_.enabled(Feature::TimestampQueries)) {
        return fail<QueryPoolHandle>(RhiErrorCode::FeatureDisabled, "create_query_pool: the TimestampQueries feature is not enabled");
    }
    auto object = std::make_shared<QueryPoolObject>();
    object->count = count;
    object->written.resize(count);
    D3D12_QUERY_HEAP_DESC heap{};
    heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap.Count = count;
    if (const HRESULT hr = device_->CreateQueryHeap(&heap, IID_PPV_ARGS(&object->heap)); FAILED(hr)) {
        return core::failure(hresult_error("create_query_pool", hr));
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * sizeof(std::uint64_t);
    if (const HRESULT hr = create_readback_resource(device_.Get(), bytes, object->readback); FAILED(hr)) {
        return core::failure(hresult_error("create_query_pool: readback buffer", hr));
    }
    void* mapped = nullptr;
    if (const HRESULT hr = object->readback->Map(0, nullptr, &mapped); FAILED(hr)) {
        return core::failure(hresult_error("create_query_pool: Map", hr));
    }
    object->values = static_cast<std::uint64_t*>(mapped);
    return core::success(query_pools_.add(std::move(object), device_id_));
}

RhiResult<std::unique_ptr<CommandList>> Dx12Device::create_command_list(QueueType queue) {
    const D3D12_COMMAND_LIST_TYPE type = kListTypes[queue_slot(queue)];
    auto data = std::make_shared<ListData>();
    if (const HRESULT hr = device_->CreateCommandAllocator(type, IID_PPV_ARGS(&data->allocator)); FAILED(hr)) {
        return core::failure(hresult_error("create_command_list: CreateCommandAllocator", hr));
    }
    if (const HRESULT hr = device_->CreateCommandList(0, type, data->allocator.Get(), nullptr, IID_PPV_ARGS(&data->list)); FAILED(hr)) {
        return core::failure(hresult_error("create_command_list: CreateCommandList", hr));
    }
    std::unique_ptr<CommandList> list = std::make_unique<Dx12CommandList>(*this, queue, std::move(data));
    return core::success(std::move(list));
}

void Dx12Device::release_descriptors(TextureObject& texture) {
    if (texture.rtv_slot) {
        rtv_pool_.release(*texture.rtv_slot);
        texture.rtv_slot.reset();
    }
    if (texture.dsv_slot) {
        dsv_pool_.release(*texture.dsv_slot);
        texture.dsv_slot.reset();
    }
}

RhiStatus Dx12Device::destroy(BufferHandle buffer) {
    std::shared_ptr<BufferObject> object = buffers_.take(buffer, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: buffer handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

RhiStatus Dx12Device::destroy(TextureHandle texture) {
    const TextureObject* live = textures_.find(texture, device_id_);
    if (live == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: texture handle is invalid or already destroyed");
    }
    if (live->swapchain_owned) {
        return fail(RhiErrorCode::InvalidState, "destroy: the swapchain owns this back buffer; destroy the swapchain instead");
    }
    std::shared_ptr<TextureObject> object = textures_.take(texture, device_id_);
    release_descriptors(*object);
    retire(std::move(object));
    return core::success();
}

RhiStatus Dx12Device::destroy(FenceHandle fence) {
    std::shared_ptr<FenceObject> object = fences_.take(fence, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: fence handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

RhiStatus Dx12Device::destroy(QueryPoolHandle pool) {
    std::shared_ptr<QueryPoolObject> object = query_pools_.take(pool, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: query pool handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

// Keeps an object until every submission that used it has completed. An object no pending work uses is released here.
void Dx12Device::retire(std::shared_ptr<ObjectBase> object) {
    object->destroyed = true;
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        const std::uint64_t value = object->last_use[slot];
        if (value != 0 && !timeline_complete(kQueueOrder[slot], value)) {
            retain(Stamp{kQueueOrder[slot], value}, {object});
        }
    }
}

void Dx12Device::retain(Stamp stamp, std::vector<std::shared_ptr<void>> objects) {
    retained_.push_back(Retained{stamp, std::move(objects)});
}

void Dx12Device::collect_completed() {
    std::erase_if(retained_, [this](const Retained& entry) {
        return timeline_complete(entry.stamp.queue, entry.stamp.value);
    });
}

RhiStatus Dx12Device::submit(QueueType queue, const std::vector<CommandList*>& lists, FenceHandle signal_fence,
                             std::uint64_t signal_value) {
    FenceObject* fence = fences_.find(signal_fence, device_id_);
    if (fence == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "submit: signal fence handle is invalid or destroyed");
    }
    if (signal_value <= fence->signaled) {
        return fail(RhiErrorCode::InvalidState, "submit: the signal value must be greater than the fence's last signaled value");
    }
    // Validate every list before any change, so a rejected submission leaves the lists and the device as they were.
    std::vector<Dx12CommandList*> recorders;
    for (CommandList* list : lists) {
        auto* recorder = dynamic_cast<Dx12CommandList*>(list);
        if (recorder == nullptr || &recorder->device_ != this) {
            return fail(RhiErrorCode::InvalidHandle, "submit: command list belongs to another device");
        }
        if (recorder->state_ != Dx12CommandList::ListState::Closed) {
            return fail(RhiErrorCode::InvalidState, "submit: command list must be closed and not yet submitted");
        }
        if (recorder->queue_ != queue) {
            return fail(RhiErrorCode::InvalidState, "submit: command list was recorded for another queue");
        }
        if (std::find(recorders.begin(), recorders.end(), recorder) != recorders.end()) {
            return fail(RhiErrorCode::InvalidState, "submit: the same command list appears twice");
        }
        for (const auto& ref : recorder->refs_) {
            if (ref->destroyed) {
                return fail(RhiErrorCode::InvalidHandle, "submit: a command list uses an object destroyed after it was recorded");
            }
        }
        recorders.push_back(recorder);
    }

    std::vector<ID3D12CommandList*> native;
    native.reserve(recorders.size());
    for (Dx12CommandList* recorder : recorders) {
        native.push_back(recorder->data_->list.Get());
    }
    const std::size_t slot = queue_slot(queue);
    queues_[slot]->ExecuteCommandLists(static_cast<UINT>(native.size()), native.data());
    const auto timeline = signal_timeline(queue);
    if (!timeline.ok()) {
        return core::failure(timeline.error());
    }
    if (const HRESULT hr = queues_[slot]->Signal(fence->fence.Get(), signal_value); FAILED(hr)) {
        return core::failure(hresult_error("submit: Signal", hr));
    }
    fence->signaled = signal_value;
    fence->last_use[slot] = timeline.value();

    // Every object the lists name is held until this submission's timeline value completes, and so is each list's D3D12 data.
    const Stamp stamp{queue, timeline.value()};
    std::vector<std::shared_ptr<void>> held{fence->shared_from_this()};
    for (Dx12CommandList* recorder : recorders) {
        for (const auto& ref : recorder->refs_) {
            ref->last_use[slot] = stamp.value;
            held.push_back(ref);
        }
        held.push_back(recorder->data_);
        for (const auto& [pool, index] : recorder->timestamps_) {
            pool->written[index] = stamp;
        }
        recorder->state_ = Dx12CommandList::ListState::Submitted;
        // The submission holds everything it names, so the list drops its own references. Otherwise a submitted list would keep
        // a descriptor set alive, and the set's heap range would not return when the set is destroyed.
        recorder->refs_.clear();
        recorder->timestamps_.clear();
        recorder->pipeline_.reset();
        recorder->ray_pipeline_.reset();
        recorder->sets_.fill(nullptr);
        recorder->bound_set_mask_ = 0;
    }
    retain(stamp, std::move(held));
    collect_completed();
    return core::success();
}

RhiResult<std::uint64_t> Dx12Device::fence_value(FenceHandle fence) {
    const FenceObject* object = fences_.find(fence, device_id_);
    if (object == nullptr) {
        return fail<std::uint64_t>(RhiErrorCode::InvalidHandle, "fence_value: fence handle is invalid or destroyed");
    }
    return core::success(object->fence->GetCompletedValue());
}

RhiStatus Dx12Device::wait_fence(FenceHandle fence, std::uint64_t value) {
    const FenceObject* object = fences_.find(fence, device_id_);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "wait_fence: fence handle is invalid or destroyed");
    }
    if (value > object->signaled) {
        return fail(RhiErrorCode::InvalidState, "wait_fence: no pending submission reaches the requested fence value");
    }
    if (object->fence->GetCompletedValue() < value) {
        if (const HRESULT hr = object->fence->SetEventOnCompletion(value, wait_event_); FAILED(hr)) {
            return core::failure(hresult_error("wait_fence: SetEventOnCompletion", hr));
        }
        WaitForSingleObject(wait_event_, INFINITE);
    }
    collect_completed();
    return core::success();
}

bool Dx12Device::timeline_complete(QueueType queue, std::uint64_t value) const {
    return timelines_[queue_slot(queue)]->GetCompletedValue() >= value;
}

RhiResult<std::uint64_t> Dx12Device::signal_timeline(QueueType queue) {
    const std::size_t slot = queue_slot(queue);
    const std::uint64_t value = ++timeline_values_[slot];
    if (const HRESULT hr = queues_[slot]->Signal(timelines_[slot].Get(), value); FAILED(hr)) {
        return core::failure(hresult_error("signal_timeline", hr));
    }
    return core::success(value);
}

RhiStatus Dx12Device::wait_timeline(QueueType queue, std::uint64_t value) {
    if (timeline_complete(queue, value)) {
        return core::success();
    }
    if (const HRESULT hr = timelines_[queue_slot(queue)]->SetEventOnCompletion(value, wait_event_); FAILED(hr)) {
        return core::failure(hresult_error("wait_timeline: SetEventOnCompletion", hr));
    }
    WaitForSingleObject(wait_event_, INFINITE);
    return core::success();
}

RhiStatus Dx12Device::wait_for_all_queues() {
    for (QueueType queue : kQueueOrder) {
        if (RhiStatus status = wait_timeline(queue, timeline_values_[queue_slot(queue)]); !status.ok()) {
            return status;
        }
    }
    return core::success();
}

RhiStatus Dx12Device::wait_idle() {
    RhiStatus status = wait_for_all_queues();
    collect_completed();
    return status;
}

RhiResult<std::uint64_t> Dx12Device::read_timestamp(QueryPoolHandle pool, std::uint32_t index) {
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
    return core::success(query->values[index]);
}

RhiStatus Dx12Device::read_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, void* destination) {
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

RhiStatus Dx12Device::upload_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, const void* source) {
    const BufferObject* object = buffers_.find(buffer, device_id_);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "upload_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(object->desc.usage, BufferUsage::CpuWrite)) {
        return fail(RhiErrorCode::InvalidDescriptor, "upload_buffer: the buffer was not created with CpuWrite usage");
    }
    if (offset > object->desc.size_bytes || size > object->desc.size_bytes - offset) {
        return fail(RhiErrorCode::OutOfRange, "upload_buffer: range is outside the buffer");
    }
    if (size == 0) {
        return core::success();
    }
    if (source == nullptr) {
        return fail(RhiErrorCode::InvalidDescriptor, "upload_buffer: source is null");
    }
    // The GPU may still read the upload memory, so the caller must wait for the submissions that used it.
    for (std::size_t slot = 0; slot < kQueueCount; ++slot) {
        if (object->last_use[slot] != 0 && !timeline_complete(kQueueOrder[slot], object->last_use[slot])) {
            return fail(RhiErrorCode::NotReady, "upload_buffer: a submission that uses the buffer has not completed");
        }
    }
    std::memcpy(static_cast<std::uint8_t*>(object->mapped) + offset, source, static_cast<std::size_t>(size));
    return core::success();
}

RhiStatus Dx12Device::queue_wait(QueueType queue, FenceHandle fence, std::uint64_t value) {
    const FenceObject* object = fences_.find(fence, device_id_);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "queue_wait: fence handle is invalid or destroyed");
    }
    if (value > object->signaled) {
        return fail(RhiErrorCode::InvalidState, "queue_wait: no submission has signaled that fence value yet");
    }
    if (const HRESULT hr = queues_[queue_slot(queue)]->Wait(object->fence.Get(), value); FAILED(hr)) {
        return core::failure(hresult_error("queue_wait: Wait", hr));
    }
    return core::success();
}

RhiResult<ResourceStats> Dx12Device::resource_stats() {
    ResourceStats stats;
    stats.buffers = static_cast<std::uint32_t>(buffers_.live());
    stats.textures = static_cast<std::uint32_t>(textures_.live());
    stats.descriptor_sets = static_cast<std::uint32_t>(sets_.live());
    stats.shader_descriptors = shader_heap_->in_use();
    stats.retained_holds = static_cast<std::uint32_t>(retained_.size());
    return core::success(stats);
}

RhiStatus Dx12Device::check_debug_messages() {
    if (!info_queue_) {
        return core::success();
    }
    std::string report;
    const UINT64 count = info_queue_->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T length = 0;
        info_queue_->GetMessage(index, nullptr, &length);
        std::vector<unsigned char> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(info_queue_->GetMessage(index, message, &length))) {
            continue;
        }
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            report += message->pDescription;
            report += '\n';
        }
    }
    info_queue_->ClearStoredMessages();
    if (!report.empty()) {
        return fail(RhiErrorCode::InvalidState, "DX12 debug layer reported errors:\n" + report);
    }
    return core::success();
}

RhiResult<std::unique_ptr<Device>> create_dx12_device(const DeviceDesc& desc) {
    auto device = std::make_unique<Dx12Device>();
    if (RhiStatus status = device->initialize(desc); !status.ok()) {
        return core::failure(status.error());
    }
    std::unique_ptr<Device> result = std::move(device);
    return core::success(std::move(result));
}

}  // namespace engine::rhi::dx12
