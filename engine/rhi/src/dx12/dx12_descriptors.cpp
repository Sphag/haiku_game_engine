// Descriptor layouts, descriptor sets, and the shader-visible heap they draw from (US-029).

#include "dx12/dx12_device.hpp"

#include "engine/core/result.hpp"

#include <algorithm>
#include <limits>

namespace engine::rhi::dx12 {
namespace {

// Largest constant buffer view D3D12 accepts.
constexpr std::uint64_t kMaxConstantBufferBytes = 64 * 1024;

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// Index of the binding named `binding` in a layout, or nothing.
std::optional<std::size_t> binding_position(const std::vector<DescriptorBindingDesc>& bindings, std::uint32_t binding) {
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (bindings[i].binding == binding) {
            return i;
        }
    }
    return std::nullopt;
}

// Whether a buffer's usage flags allow a descriptor type. The same rule as the reference device.
bool buffer_usage_matches(BufferUsage usage, DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return has_flags(usage, BufferUsage::Constant);
        case DescriptorType::ShaderResource:
            return has_flags(usage, BufferUsage::ShaderResource);
        case DescriptorType::UnorderedAccess:
            return has_flags(usage, BufferUsage::UnorderedAccess);
    }
    return false;
}

// Whether a texture's usage flags allow a descriptor type. Constant buffers are buffers only.
bool texture_usage_matches(TextureUsage usage, DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return false;
        case DescriptorType::ShaderResource:
            return has_flags(usage, TextureUsage::ShaderResource);
        case DescriptorType::UnorderedAccess:
            return has_flags(usage, TextureUsage::UnorderedAccess);
    }
    return false;
}

// Formats an unordered access view can use in D3D12. sRGB, BGRA, and depth formats have no typed UAV.
bool uav_format_allowed(Format format) noexcept {
    switch (format) {
        case Format::R8G8B8A8Unorm:
        case Format::R16G16B16A16Float:
        case Format::R32Float:
        case Format::R32Uint:
            return true;
        case Format::Unknown:
        case Format::R8G8B8A8Srgb:
        case Format::B8G8R8A8Unorm:
        case Format::D32Float:
        case Format::R32G32B32Float:
        case Format::R32G32Float:
            return false;
    }
    return false;
}

// Typed view format for a texture's shader-read view. A depth texture is created typeless, so its color view reads R32_FLOAT.
DXGI_FORMAT srv_format(Format format) noexcept {
    return is_depth_format(format) ? DXGI_FORMAT_R32_FLOAT : to_dxgi(format);
}

}  // namespace

// ---- ShaderDescriptorHeap ----

RhiStatus ShaderDescriptorHeap::initialize(ID3D12Device* device, std::uint32_t capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
    if (FAILED(hr)) {
        return core::failure(hresult_error("create_device: CreateDescriptorHeap (shader-visible)", hr));
    }
    cpu_start_ = heap_->GetCPUDescriptorHandleForHeapStart();
    gpu_start_ = heap_->GetGPUDescriptorHandleForHeapStart();
    increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    capacity_ = capacity;
    free_.push_back(Range{0, capacity});
    return core::success();
}

std::optional<std::uint32_t> ShaderDescriptorHeap::allocate(std::uint32_t count) {
    if (count == 0) {
        return std::nullopt;
    }
    // First fit: the lowest range that can hold the request keeps the heap compact.
    for (auto range = free_.begin(); range != free_.end(); ++range) {
        if (range->count < count) {
            continue;
        }
        const std::uint32_t first = range->first;
        range->first += count;
        range->count -= count;
        if (range->count == 0) {
            free_.erase(range);
        }
        in_use_ += count;
        return first;
    }
    return std::nullopt;
}

void ShaderDescriptorHeap::release(std::uint32_t first, std::uint32_t count) {
    const auto next = std::lower_bound(free_.begin(), free_.end(), first,
                                       [](const Range& range, std::uint32_t value) { return range.first < value; });
    auto inserted = free_.insert(next, Range{first, count});
    // Merge with the following range, then the preceding one, so the free list never holds two adjoining ranges.
    if (inserted + 1 != free_.end() && inserted->first + inserted->count == (inserted + 1)->first) {
        inserted->count += (inserted + 1)->count;
        free_.erase(inserted + 1);
    }
    if (inserted != free_.begin() && (inserted - 1)->first + (inserted - 1)->count == inserted->first) {
        (inserted - 1)->count += inserted->count;
        free_.erase(inserted);
    }
    in_use_ -= count;
}

D3D12_CPU_DESCRIPTOR_HANDLE ShaderDescriptorHeap::cpu(std::uint32_t slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_start_;
    handle.ptr += static_cast<SIZE_T>(slot) * increment_;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE ShaderDescriptorHeap::gpu(std::uint32_t slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = gpu_start_;
    handle.ptr += static_cast<UINT64>(slot) * increment_;
    return handle;
}

// ---- DescriptorSetObject ----

DescriptorSetObject::~DescriptorSetObject() {
    // The range returns to the heap only here, after the last submission that named the set has completed.
    if (heap) {
        heap->release(first_slot, static_cast<std::uint32_t>(elements.size()));
    }
}

// ---- Dx12Device ----

DescriptorSetLayoutObject* Dx12Device::find_layout(DescriptorSetLayoutHandle handle) const {
    return layouts_.find(handle, device_id_);
}

DescriptorSetObject* Dx12Device::find_set(DescriptorSetHandle handle) const {
    return sets_.find(handle, device_id_);
}

RhiResult<DescriptorSetLayoutHandle> Dx12Device::create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) {
    if (desc.bindings.empty()) {
        return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "create_descriptor_set_layout: a layout needs bindings");
    }
    auto object = std::make_shared<DescriptorSetLayoutObject>();
    for (std::size_t i = 0; i < desc.bindings.size(); ++i) {
        const DescriptorBindingDesc& binding = desc.bindings[i];
        if (binding.count == 0) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "descriptor binding count must be at least one");
        }
        if (binding.visibility == ShaderStage::None) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "descriptor binding needs a shader stage");
        }
        if (binding_position(object->bindings, binding.binding)) {
            return fail<DescriptorSetLayoutHandle>(RhiErrorCode::InvalidDescriptor, "descriptor layout repeats a binding index");
        }
        object->first_element.push_back(object->element_count);
        object->element_count += binding.count;
        object->bindings.push_back(binding);
    }
    return core::success(layouts_.add(std::move(object), device_id_));
}

RhiResult<DescriptorSetHandle> Dx12Device::create_descriptor_set(DescriptorSetLayoutHandle layout) {
    const DescriptorSetLayoutObject* source = find_layout(layout);
    if (source == nullptr) {
        return fail<DescriptorSetHandle>(RhiErrorCode::InvalidHandle, "create_descriptor_set: layout handle is invalid or destroyed");
    }
    auto object = std::make_shared<DescriptorSetObject>();
    object->bindings = source->bindings;
    object->first_element = source->first_element;
    object->elements.resize(source->element_count);
    const auto first = shader_heap_->allocate(source->element_count);
    if (!first) {
        return fail<DescriptorSetHandle>(RhiErrorCode::OutOfRange, "create_descriptor_set: no shader-visible descriptors left");
    }
    object->heap = shader_heap_;
    object->first_slot = *first;
    return core::success(sets_.add(std::move(object), device_id_));
}

RhiStatus Dx12Device::write_buffer(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, BufferHandle buffer) {
    DescriptorSetObject* target = find_set(set);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_buffer: descriptor set handle is invalid or destroyed");
    }
    BufferObject* resource = buffers_.find(buffer, device_id_);
    if (resource == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_buffer: buffer handle is invalid or destroyed");
    }
    const std::optional<std::size_t> position = binding_position(target->bindings, binding);
    if (!position) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_buffer: the set has no such binding");
    }
    const DescriptorBindingDesc& desc = target->bindings[*position];
    if (array_index >= desc.count) {
        return fail(RhiErrorCode::OutOfRange, "write_buffer: array index is outside the binding");
    }
    if (!buffer_usage_matches(resource->desc.usage, desc.type)) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_buffer: buffer usage does not match the binding type");
    }

    const std::uint32_t element = target->first_element[*position] + array_index;
    const std::uint32_t slot = target->first_slot + element;
    const std::uint64_t size = resource->desc.size_bytes;
    switch (desc.type) {
        case DescriptorType::ConstantBuffer: {
            const std::uint64_t view_bytes = align_up(size, 256);
            if (view_bytes > kMaxConstantBufferBytes) {
                return fail(RhiErrorCode::InvalidDescriptor, "write_buffer: a constant buffer view is limited to 64 KiB");
            }
            D3D12_CONSTANT_BUFFER_VIEW_DESC view{};
            view.BufferLocation = resource->resource->GetGPUVirtualAddress();
            view.SizeInBytes = static_cast<UINT>(view_bytes);
            device_->CreateConstantBufferView(&view, shader_heap_->cpu(slot));
            break;
        }
        case DescriptorType::ShaderResource:
        case DescriptorType::UnorderedAccess: {
            // Raw views: the shader addresses the buffer in bytes, so the size must be a whole number of 32-bit words.
            if (size % 4 != 0 || size / 4 > std::numeric_limits<UINT>::max()) {
                return fail(RhiErrorCode::InvalidDescriptor, "write_buffer: a buffer view needs a size that is a multiple of four bytes");
            }
            const auto words = static_cast<UINT>(size / 4);
            if (desc.type == DescriptorType::ShaderResource) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R32_TYPELESS;
                view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Buffer.NumElements = words;
                view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                device_->CreateShaderResourceView(resource->resource.Get(), &view, shader_heap_->cpu(slot));
            } else {
                D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R32_TYPELESS;
                view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                view.Buffer.NumElements = words;
                view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                device_->CreateUnorderedAccessView(resource->resource.Get(), nullptr, &view, shader_heap_->cpu(slot));
            }
            break;
        }
    }
    DescriptorElement& entry = target->elements[element];
    entry.type = desc.type;
    entry.buffer = share(resource);
    entry.texture.reset();
    entry.acceleration.reset();
    return core::success();
}

RhiStatus Dx12Device::write_acceleration_structure(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index,
                                                   AccelerationStructureHandle structure) {
    DescriptorSetObject* target = find_set(set);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_acceleration_structure: descriptor set handle is invalid or destroyed");
    }
    AccelerationStructureObject* resource = acceleration_structures_.find(structure, device_id_);
    if (resource == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_acceleration_structure: structure handle is invalid or destroyed");
    }
    if (!resource->top_level) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_acceleration_structure: only a top-level structure can be bound");
    }
    const std::optional<std::size_t> position = binding_position(target->bindings, binding);
    if (!position) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_acceleration_structure: the set has no such binding");
    }
    const DescriptorBindingDesc& desc = target->bindings[*position];
    if (desc.type != DescriptorType::ShaderResource) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_acceleration_structure: the binding must be a ShaderResource");
    }
    if (array_index >= desc.count) {
        return fail(RhiErrorCode::OutOfRange, "write_acceleration_structure: array index is outside the binding");
    }

    // The view carries the structure's GPU address; the structure is not a resource, so no resource pointer is given.
    const std::uint32_t element = target->first_element[*position] + array_index;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.RaytracingAccelerationStructure.Location = resource->address;
    device_->CreateShaderResourceView(nullptr, &view, shader_heap_->cpu(target->first_slot + element));

    DescriptorElement& entry = target->elements[element];
    entry.type = desc.type;
    entry.buffer.reset();
    entry.texture.reset();
    entry.acceleration = share(resource);
    return core::success();
}

RhiStatus Dx12Device::write_texture(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, TextureHandle texture) {
    DescriptorSetObject* target = find_set(set);
    if (target == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_texture: descriptor set handle is invalid or destroyed");
    }
    TextureObject* resource = textures_.find(texture, device_id_);
    if (resource == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_texture: texture handle is invalid or destroyed");
    }
    const std::optional<std::size_t> position = binding_position(target->bindings, binding);
    if (!position) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_texture: the set has no such binding");
    }
    const DescriptorBindingDesc& desc = target->bindings[*position];
    if (array_index >= desc.count) {
        return fail(RhiErrorCode::OutOfRange, "write_texture: array index is outside the binding");
    }
    if (desc.type == DescriptorType::ConstantBuffer) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_texture: a constant buffer binding takes a buffer");
    }
    if (!texture_usage_matches(resource->desc.usage, desc.type)) {
        return fail(RhiErrorCode::InvalidDescriptor, "write_texture: texture usage does not match the binding type");
    }

    const std::uint32_t element = target->first_element[*position] + array_index;
    const std::uint32_t slot = target->first_slot + element;
    if (desc.type == DescriptorType::ShaderResource) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = srv_format(resource->desc.format);
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = resource->desc.mip_levels;
        device_->CreateShaderResourceView(resource->resource.Get(), &view, shader_heap_->cpu(slot));
    } else {
        if (!uav_format_allowed(resource->desc.format)) {
            return fail(RhiErrorCode::InvalidDescriptor, "write_texture: the texture format has no unordered access view");
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = to_dxgi(resource->desc.format);
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;
        device_->CreateUnorderedAccessView(resource->resource.Get(), nullptr, &view, shader_heap_->cpu(slot));
    }
    DescriptorElement& entry = target->elements[element];
    entry.type = desc.type;
    entry.texture = share(resource);
    entry.buffer.reset();
    entry.acceleration.reset();
    return core::success();
}

RhiStatus Dx12Device::destroy(DescriptorSetLayoutHandle layout) {
    // A layout is never named by a submission; sets and pipelines copy what they need from it.
    if (!layouts_.take(layout, device_id_)) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: descriptor set layout handle is invalid or already destroyed");
    }
    return core::success();
}

RhiStatus Dx12Device::destroy(DescriptorSetHandle set) {
    std::shared_ptr<DescriptorSetObject> object = sets_.take(set, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: descriptor set handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

}  // namespace engine::rhi::dx12
