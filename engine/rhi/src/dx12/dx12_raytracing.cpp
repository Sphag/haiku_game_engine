// Ray tracing foundation for DX12 (US-041): bottom- and top-level acceleration structures, ray tracing state objects with their shader
// tables, and the list commands that build them and launch rays. Every object is kept by the submissions that name it, so destroying one
// during a frame that still uses it is safe: its memory is released at that frame's fence (ObjectBase::last_use).

#include "dx12/dx12_command_list.hpp"

#include "engine/core/result.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::rhi::dx12 {
namespace {

// Each shader table record is one slot of this size, and a record starts on a slot boundary (the table alignment is 64 bytes).
constexpr std::uint64_t kShaderRecordStride = 64;
constexpr std::uint64_t kRayGenRecord = 0;
constexpr std::uint64_t kMissRecord = 1;
constexpr std::uint64_t kHitGroupRecord = 2;
constexpr std::uint64_t kShaderTableBytes = 3 * kShaderRecordStride;
// The hit group name inside the state object. Pipelines have one hit group, so the name is fixed.
constexpr wchar_t kHitGroupName[] = L"EngineHitGroup";
// Dispatch dimensions stay within the 16-bit group limit that the compute path uses.
constexpr std::uint32_t kMaxDispatchDimension = 65535;
// A scratch buffer is never empty, so an update-only build still has memory to read from.
constexpr std::uint64_t kMinScratchBytes = 256;

// Entry names are ASCII identifiers in the HLSL source, so a byte-wise widening is exact.
std::wstring to_wide(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

// Default-heap memory for an acceleration structure or its scratch. Both are written by the GPU, so they need unordered-access flags.
RhiResult<ComPtr<ID3D12Resource>> create_default_memory(ID3D12Device* device, std::uint64_t size_bytes, D3D12_RESOURCE_STATES initial) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    if (const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&resource));
        FAILED(hr)) {
        return core::failure(hresult_error("acceleration structure memory: CreateCommittedResource", hr));
    }
    return core::success(resource);
}

// Build flags for one structure. Refit builds update in place and need the structure to have been built with ALLOW_UPDATE.
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS build_flags(bool allow_refit, bool refit) noexcept {
    UINT flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (allow_refit) {
        flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }
    if (refit) {
        flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }
    return static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(flags);
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom_inputs(const AccelerationStructureObject& object, bool refit) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = build_flags(object.allow_refit, refit);
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &object.geometry;
    return inputs;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top_inputs(const AccelerationStructureObject& object, bool refit,
                                                              std::uint32_t instance_count) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = build_flags(object.allow_refit, refit);
    inputs.NumDescs = instance_count;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = object.instances.Get() != nullptr ? object.instances->GetGPUVirtualAddress() : 0;
    return inputs;
}

// Sizes the result and scratch memory of a new structure from the driver's prebuild estimate.
RhiStatus allocate_structure(ID3D12Device* device, AccelerationStructureObject& object, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& info) {
    auto result = create_default_memory(device, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    if (!result.ok()) {
        return core::failure(result.error());
    }
    const std::uint64_t scratch_bytes = std::max<std::uint64_t>({info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes, kMinScratchBytes});
    auto scratch = create_default_memory(device, scratch_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!scratch.ok()) {
        return core::failure(scratch.error());
    }
    object.result = std::move(result).value();
    object.scratch = std::move(scratch).value();
    object.address = object.result->GetGPUVirtualAddress();
    return core::success();
}

// A UAV barrier after a build, so the next build or trace reads the structure only after it is written.
void uav_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}

// The DXR command interface. It is present on every device that reports ray tracing (see Dx12Device::initialize).
RhiResult<ComPtr<ID3D12GraphicsCommandList4>> raytracing_list(ID3D12GraphicsCommandList* list, std::string_view operation) {
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (const HRESULT hr = list->QueryInterface(IID_PPV_ARGS(&list4)); FAILED(hr)) {
        return core::failure(hresult_error(std::string(operation) + ": ID3D12GraphicsCommandList4", hr));
    }
    return core::success(list4);
}

}  // namespace

// ---- Dx12Device: acceleration structures ----

AccelerationStructureObject* Dx12Device::find_acceleration(AccelerationStructureHandle handle) const {
    return acceleration_structures_.find(handle, device_id_);
}

RayTracingPipelineObject* Dx12Device::find_ray_pipeline(RayTracingPipelineHandle handle) const {
    return ray_pipelines_.find(handle, device_id_);
}

RhiResult<AccelerationStructureHandle> Dx12Device::create_bottom_level(const BottomLevelDesc& desc) {
    if (!report_.enabled(Feature::RayTracing)) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::FeatureDisabled, "create_bottom_level: the RayTracing feature is not enabled");
    }
    BufferObject* vertices = find_buffer(desc.vertex_buffer);
    if (vertices == nullptr) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidHandle, "create_bottom_level: vertex buffer handle is invalid or destroyed");
    }
    // DXR reads geometry as shader input, so both buffers need ShaderResource usage to be in the ShaderRead state a build expects.
    if (!has_flags(vertices->desc.usage, BufferUsage::Vertex | BufferUsage::ShaderResource)) {
        return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor,
                                                 "create_bottom_level: the vertex buffer needs Vertex and ShaderResource usage");
    }
    // The float3 position must fit inside a vertex, and the stride and offset stay on 4-byte boundaries, as DXR requires.
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
        if (static_cast<std::uint64_t>(desc.index_count) * index_stride_bytes(desc.index_format) > indices->desc.size_bytes) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::OutOfRange, "create_bottom_level: the index count exceeds the index buffer");
        }
    } else {
        if (desc.index_count != 0) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor, "create_bottom_level: an index count needs an index buffer");
        }
        if (desc.vertex_count % 3 != 0) {
            return fail<AccelerationStructureHandle>(RhiErrorCode::InvalidDescriptor, "create_bottom_level: non-indexed geometry needs a vertex count that is a multiple of three");
        }
    }

    auto object = std::make_shared<AccelerationStructureObject>();
    object->allow_refit = desc.allow_refit;
    object->vertex_buffer = share(vertices);
    D3D12_RAYTRACING_GEOMETRY_DESC& geometry = object->geometry;
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = desc.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geometry.Triangles.Transform3x4 = 0;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = desc.vertex_count;
    geometry.Triangles.VertexBuffer.StartAddress = vertices->resource->GetGPUVirtualAddress() + desc.position_offset;
    geometry.Triangles.VertexBuffer.StrideInBytes = desc.vertex_stride;
    if (indices != nullptr) {
        object->index_buffer = share(indices);
        geometry.Triangles.IndexFormat = desc.index_format == IndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        geometry.Triangles.IndexCount = desc.index_count;
        geometry.Triangles.IndexBuffer = indices->resource->GetGPUVirtualAddress();
    } else {
        geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        geometry.Triangles.IndexCount = 0;
        geometry.Triangles.IndexBuffer = 0;
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = bottom_inputs(*object, false);
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (const RhiStatus allocated = allocate_structure(device_.Get(), *object, info); !allocated.ok()) {
        return core::failure(allocated.error());
    }
    set_name(object->result.Get(), desc.debug_name);
    return core::success(acceleration_structures_.add(std::move(object), device_id_));
}

RhiResult<AccelerationStructureHandle> Dx12Device::create_top_level(const TopLevelDesc& desc) {
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
    const std::uint64_t instance_bytes = static_cast<std::uint64_t>(desc.max_instances) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (const HRESULT hr = create_upload_resource(device_.Get(), instance_bytes, object->instances); FAILED(hr)) {
        return core::failure(hresult_error("create_top_level: instance upload memory", hr));
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = top_inputs(*object, false, desc.max_instances);
    device5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (const RhiStatus allocated = allocate_structure(device_.Get(), *object, info); !allocated.ok()) {
        return core::failure(allocated.error());
    }
    set_name(object->result.Get(), desc.debug_name);
    return core::success(acceleration_structures_.add(std::move(object), device_id_));
}

RhiStatus Dx12Device::write_top_level(AccelerationStructureHandle structure, const std::vector<TopLevelInstance>& instances) {
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
    // The GPU may still read the instance memory of a pending build, so the caller waits for those submissions first.
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
        references.push_back(share(bottom));
    }

    void* mapped = nullptr;
    const D3D12_RANGE nothing_read{0, 0};
    if (const HRESULT hr = object->instances->Map(0, &nothing_read, &mapped); FAILED(hr)) {
        return core::failure(hresult_error("write_top_level: Map", hr));
    }
    auto* records = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(mapped);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const TopLevelInstance& instance = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC record{};
        std::memcpy(record.Transform, instance.transform.data(), sizeof record.Transform);
        record.InstanceID = instance.instance_id & 0xFFFFFFu;
        record.InstanceMask = static_cast<UINT>(instance.mask);
        record.InstanceContributionToHitGroupIndex = 0;
        record.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        record.AccelerationStructure = references[i]->address;
        records[i] = record;
    }
    object->instances->Unmap(0, nullptr);
    object->references = std::move(references);
    object->instance_count = static_cast<std::uint32_t>(instances.size());
    object->written = true;
    return core::success();
}

RhiStatus Dx12Device::destroy(AccelerationStructureHandle structure) {
    std::shared_ptr<AccelerationStructureObject> object = acceleration_structures_.take(structure, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: acceleration structure handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

// ---- Dx12Device: ray tracing pipelines ----

RhiResult<RayTracingPipelineHandle> Dx12Device::create_ray_tracing_pipeline(const RayTracingPipelineDesc& desc) {
    if (!report_.enabled(Feature::RayTracing)) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::FeatureDisabled, "create_ray_tracing_pipeline: the RayTracing feature is not enabled");
    }
    if (desc.set_layouts.size() > kMaxDescriptorSetsPerPipeline) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::OutOfRange, "create_ray_tracing_pipeline: too many descriptor set layouts");
    }
    if (desc.library.bytecode.empty() || desc.raygen_entry.empty() || desc.miss_entry.empty() || desc.closest_hit_entry.empty()) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidDescriptor,
                                              "create_ray_tracing_pipeline: needs a DXIL library and ray generation, miss, and closest-hit entries");
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

    std::vector<std::vector<DescriptorBindingDesc>> layouts;
    for (const DescriptorSetLayoutHandle& layout : desc.set_layouts) {
        const DescriptorSetLayoutObject* source = find_layout(layout);
        if (source == nullptr) {
            return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidHandle, "create_ray_tracing_pipeline: descriptor set layout handle is invalid or destroyed");
        }
        layouts.push_back(source->bindings);
    }
    auto root = build_root_signature(device_.Get(), layouts);
    if (!root.ok()) {
        return core::failure(root.error());
    }

    const std::wstring raygen = to_wide(desc.raygen_entry);
    const std::wstring miss = to_wide(desc.miss_entry);
    const std::wstring closest_hit = to_wide(desc.closest_hit_entry);
    const std::array<D3D12_EXPORT_DESC, 3> exports{
        D3D12_EXPORT_DESC{raygen.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE},
        D3D12_EXPORT_DESC{miss.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE},
        D3D12_EXPORT_DESC{closest_hit.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE},
    };
    D3D12_DXIL_LIBRARY_DESC library{};
    library.DXILLibrary = D3D12_SHADER_BYTECODE{desc.library.bytecode.data(), desc.library.bytecode.size()};
    library.NumExports = static_cast<UINT>(exports.size());
    library.pExports = exports.data();
    D3D12_HIT_GROUP_DESC hit_group{};
    hit_group.HitGroupExport = kHitGroupName;
    hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hit_group.ClosestHitShaderImport = closest_hit.c_str();
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
    shader_config.MaxPayloadSizeInBytes = desc.max_payload_bytes;
    shader_config.MaxAttributeSizeInBytes = desc.max_attribute_bytes;
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{};
    pipeline_config.MaxTraceRecursionDepth = desc.max_recursion_depth;
    D3D12_GLOBAL_ROOT_SIGNATURE global_root{root.value().Get()};

    const std::array<D3D12_STATE_SUBOBJECT, 5> subobjects{
        D3D12_STATE_SUBOBJECT{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        D3D12_STATE_SUBOBJECT{D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_group},
        D3D12_STATE_SUBOBJECT{D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        D3D12_STATE_SUBOBJECT{D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
        D3D12_STATE_SUBOBJECT{D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_root},
    };
    const D3D12_STATE_OBJECT_DESC state_desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, static_cast<UINT>(subobjects.size()), subobjects.data()};
    ComPtr<ID3D12StateObject> state;
    if (const HRESULT hr = device5_->CreateStateObject(&state_desc, IID_PPV_ARGS(&state)); FAILED(hr)) {
        return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidDescriptor,
                                              "create_ray_tracing_pipeline: CreateStateObject: " + hresult_text(hr) +
                                                  " (check the entry names and the DXIL library)");
    }

    // The three shader identifiers go into the table once; they do not change for the pipeline's life.
    ComPtr<ID3D12StateObjectProperties> properties;
    if (const HRESULT hr = state.As(&properties); FAILED(hr)) {
        return core::failure(hresult_error("create_ray_tracing_pipeline: ID3D12StateObjectProperties", hr));
    }
    const std::array<const void*, kShaderTableBytes / kShaderRecordStride> identifiers{
        properties->GetShaderIdentifier(raygen.c_str()),
        properties->GetShaderIdentifier(miss.c_str()),
        properties->GetShaderIdentifier(kHitGroupName),
    };
    for (const void* identifier : identifiers) {
        if (identifier == nullptr) {
            return fail<RayTracingPipelineHandle>(RhiErrorCode::InvalidDescriptor, "create_ray_tracing_pipeline: an entry name is not exported by the library");
        }
    }
    auto object = std::make_shared<RayTracingPipelineObject>();
    if (const HRESULT hr = create_upload_resource(device_.Get(), kShaderTableBytes, object->shader_table); FAILED(hr)) {
        return core::failure(hresult_error("create_ray_tracing_pipeline: shader table", hr));
    }
    void* mapped = nullptr;
    const D3D12_RANGE nothing_read{0, 0};
    if (const HRESULT hr = object->shader_table->Map(0, &nothing_read, &mapped); FAILED(hr)) {
        return core::failure(hresult_error("create_ray_tracing_pipeline: Map", hr));
    }
    auto* table = static_cast<std::uint8_t*>(mapped);
    for (std::size_t record = 0; record < identifiers.size(); ++record) {
        std::memcpy(table + record * kShaderRecordStride, identifiers[record], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    object->shader_table->Unmap(0, nullptr);

    object->root_signature = std::move(root).value();
    object->state = std::move(state);
    object->layouts = std::move(layouts);
    object->table_address = object->shader_table->GetGPUVirtualAddress();
    set_name(object->state.Get(), desc.debug_name);
    return core::success(ray_pipelines_.add(std::move(object), device_id_));
}

RhiStatus Dx12Device::destroy(RayTracingPipelineHandle pipeline) {
    std::shared_ptr<RayTracingPipelineObject> object = ray_pipelines_.take(pipeline, device_id_);
    if (!object) {
        return fail(RhiErrorCode::InvalidHandle, "destroy: ray tracing pipeline handle is invalid or already destroyed");
    }
    retire(std::move(object));
    return core::success();
}

// ---- Dx12CommandList: ray tracing commands ----

RhiStatus Dx12CommandList::build_bottom_level(AccelerationStructureHandle structure, BuildMode mode) {
    if (const RhiStatus open = require_recording("build_bottom_level"); !open.ok()) {
        return open;
    }
    if (queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, "build_bottom_level: needs the graphics or compute queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "build_bottom_level: a render pass is active");
    }
    AccelerationStructureObject* object = device_.find_acceleration(structure);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "build_bottom_level: structure handle is invalid or destroyed");
    }
    if (object->top_level) {
        return fail(RhiErrorCode::InvalidDescriptor, "build_bottom_level: the structure is top-level");
    }
    const bool refit = mode == BuildMode::Refit;
    if (refit && (!object->allow_refit || !object->built)) {
        return fail(RhiErrorCode::InvalidState, "build_bottom_level: a refit needs allow_refit and an earlier build");
    }
    // The geometry's buffers are read by the GPU during the build, so they must be alive and in the ShaderRead state. DXR rejects
    // the vertex and index states that the draw path uses.
    if (object->vertex_buffer->destroyed) {
        return fail(RhiErrorCode::InvalidHandle, "build_bottom_level: the vertex buffer was destroyed");
    }
    if (object->vertex_buffer->state != ResourceState::ShaderRead) {
        return fail(RhiErrorCode::StateMismatch, "build_bottom_level: the vertex buffer must be in ShaderRead state");
    }
    if (object->index_buffer) {
        if (object->index_buffer->destroyed) {
            return fail(RhiErrorCode::InvalidHandle, "build_bottom_level: the index buffer was destroyed");
        }
        if (object->index_buffer->state != ResourceState::ShaderRead) {
            return fail(RhiErrorCode::StateMismatch, "build_bottom_level: the index buffer must be in ShaderRead state");
        }
    }
    auto list4 = raytracing_list(data_->list.Get(), "build_bottom_level");
    if (!list4.ok()) {
        return core::failure(list4.error());
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.DestAccelerationStructureData = object->address;
    build.Inputs = bottom_inputs(*object, refit);
    build.ScratchAccelerationStructureData = object->scratch->GetGPUVirtualAddress();
    build.SourceAccelerationStructureData = refit ? object->address : 0;
    list4.value()->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    uav_barrier(data_->list.Get(), object->result.Get());

    object->built = true;
    use(*object);
    use(*object->vertex_buffer);
    if (object->index_buffer) {
        use(*object->index_buffer);
    }
    return core::success();
}

RhiStatus Dx12CommandList::build_top_level(AccelerationStructureHandle structure, BuildMode mode) {
    if (const RhiStatus open = require_recording("build_top_level"); !open.ok()) {
        return open;
    }
    if (queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, "build_top_level: needs the graphics or compute queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "build_top_level: a render pass is active");
    }
    AccelerationStructureObject* object = device_.find_acceleration(structure);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "build_top_level: structure handle is invalid or destroyed");
    }
    if (!object->top_level) {
        return fail(RhiErrorCode::InvalidDescriptor, "build_top_level: the structure is bottom-level");
    }
    if (!object->written) {
        return fail(RhiErrorCode::InvalidState, "build_top_level: write the instances before building");
    }
    const bool refit = mode == BuildMode::Refit;
    if (refit && (!object->allow_refit || !object->built)) {
        return fail(RhiErrorCode::InvalidState, "build_top_level: a refit needs allow_refit and an earlier build");
    }
    if (refit && object->instance_count != object->built_instances) {
        return fail(RhiErrorCode::InvalidState, "build_top_level: a refit keeps the instance count of the last build; use a full build");
    }
    // Each instance's bottom-level structure must be alive and built; the GPU reads its memory during this build.
    for (const auto& reference : object->references) {
        if (reference->destroyed) {
            return fail(RhiErrorCode::InvalidHandle, "build_top_level: an instance's bottom-level structure was destroyed");
        }
        if (!reference->built) {
            return fail(RhiErrorCode::InvalidState, "build_top_level: an instance's bottom-level structure has not been built");
        }
    }
    auto list4 = raytracing_list(data_->list.Get(), "build_top_level");
    if (!list4.ok()) {
        return core::failure(list4.error());
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.DestAccelerationStructureData = object->address;
    build.Inputs = top_inputs(*object, refit, object->instance_count);
    build.ScratchAccelerationStructureData = object->scratch->GetGPUVirtualAddress();
    build.SourceAccelerationStructureData = refit ? object->address : 0;
    list4.value()->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    uav_barrier(data_->list.Get(), object->result.Get());

    object->built = true;
    object->built_instances = object->instance_count;
    use(*object);
    // The list names every bottom-level structure the build reads, so a destroyed one is rejected at submit, not read.
    for (const auto& reference : object->references) {
        use(*reference);
    }
    return core::success();
}

RhiStatus Dx12CommandList::set_ray_tracing_pipeline(RayTracingPipelineHandle pipeline) {
    if (const RhiStatus open = require_recording("set_ray_tracing_pipeline"); !open.ok()) {
        return open;
    }
    if (queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, "set_ray_tracing_pipeline: needs the graphics or compute queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "set_ray_tracing_pipeline: a render pass is active");
    }
    RayTracingPipelineObject* object = device_.find_ray_pipeline(pipeline);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_ray_tracing_pipeline: pipeline handle is invalid or destroyed");
    }
    auto list4 = raytracing_list(data_->list.Get(), "set_ray_tracing_pipeline");
    if (!list4.ok()) {
        return core::failure(list4.error());
    }
    // Ray tracing uses the compute root signature, and its descriptor tables are bound the way compute tables are.
    data_->list->SetComputeRootSignature(object->root_signature.Get());
    list4.value()->SetPipelineState1(object->state.Get());
    // The ray tracing pipeline replaces any graphics or compute pipeline, and an earlier set binding does not carry over.
    pipeline_.reset();
    ray_pipeline_ = share(object);
    bound_set_mask_ = 0;
    sets_.fill(nullptr);
    use(*object);
    return core::success();
}

RhiStatus Dx12CommandList::dispatch_rays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    if (const RhiStatus open = require_recording("dispatch_rays"); !open.ok()) {
        return open;
    }
    if (queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, "dispatch_rays: needs the graphics or compute queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "dispatch_rays: a render pass is active");
    }
    if (!ray_pipeline_) {
        return fail(RhiErrorCode::InvalidState, "dispatch_rays: bind a ray tracing pipeline first");
    }
    if (width == 0 || height == 0 || depth == 0) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_rays: every dimension must be greater than zero");
    }
    if (width > kMaxDispatchDimension || height > kMaxDispatchDimension || depth > kMaxDispatchDimension) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_rays: a dimension exceeds 65535");
    }
    if (const RhiStatus bound = check_bound_sets("dispatch_rays"); !bound.ok()) {
        return bound;
    }
    auto list4 = raytracing_list(data_->list.Get(), "dispatch_rays");
    if (!list4.ok()) {
        return core::failure(list4.error());
    }
    // The three records sit in one table, so each range starts at its slot and every record has the same stride.
    const D3D12_GPU_VIRTUAL_ADDRESS table = ray_pipeline_->table_address;
    D3D12_DISPATCH_RAYS_DESC rays{};
    rays.RayGenerationShaderRecord = D3D12_GPU_VIRTUAL_ADDRESS_RANGE{table + kRayGenRecord * kShaderRecordStride, kShaderRecordStride};
    rays.MissShaderTable = D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{table + kMissRecord * kShaderRecordStride, kShaderRecordStride,
                                                                     kShaderRecordStride};
    rays.HitGroupTable = D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{table + kHitGroupRecord * kShaderRecordStride, kShaderRecordStride,
                                                                   kShaderRecordStride};
    rays.Width = width;
    rays.Height = height;
    rays.Depth = depth;
    list4.value()->DispatchRays(&rays);
    return core::success();
}

}  // namespace engine::rhi::dx12
