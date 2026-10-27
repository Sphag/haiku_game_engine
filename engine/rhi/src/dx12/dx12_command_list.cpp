#include "dx12/dx12_command_list.hpp"

#include "fsr2_common.hpp"
#if ENGINE_FSR2_AVAILABLE
#include "dx12/dx12_upscaler.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>

namespace engine::rhi::dx12 {
namespace {

void transition_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, ResourceState before, ResourceState after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = to_d3d(before);
    barrier.Transition.StateAfter = to_d3d(after);
    list->ResourceBarrier(1, &barrier);
}

// The state a bound descriptor needs its resource in while a dispatch runs. The same mapping as the reference device.
ResourceState descriptor_state(DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return ResourceState::ConstantBuffer;
        case DescriptorType::ShaderResource:
            return ResourceState::ShaderRead;
        case DescriptorType::UnorderedAccess:
            return ResourceState::UnorderedAccess;
    }
    return ResourceState::Common;
}

}  // namespace

Dx12CommandList::Dx12CommandList(Dx12Device& device, QueueType queue, std::shared_ptr<ListData> data)
    : device_(device), queue_(queue), data_(std::move(data)) {}

QueueType Dx12CommandList::queue() const noexcept {
    return queue_;
}

RhiStatus Dx12CommandList::require_recording(std::string_view operation) const {
    if (state_ != ListState::Recording) {
        return fail(RhiErrorCode::InvalidState,
                    std::string(operation) + ": the command list is not recording; it was closed or already submitted");
    }
    return core::success();
}

void Dx12CommandList::use(ObjectBase& object) {
    refs_.push_back(object.shared_from_this());
}

RhiStatus Dx12CommandList::transition(BufferHandle buffer, ResourceState before, ResourceState after) {
    if (const RhiStatus open = require_recording("transition"); !open.ok()) {
        return open;
    }
    BufferObject* record = device_.find_buffer(buffer);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "transition: buffer handle is invalid or destroyed");
    }
    if (record->state != before) {
        return fail(RhiErrorCode::StateMismatch, "transition: buffer is not in the state the caller named as 'before'");
    }
    if (!buffer_state_allowed(record->desc.usage, after)) {
        return fail(RhiErrorCode::InvalidDescriptor, "transition: the buffer usage does not allow the target state");
    }
    if (before != after) {
        transition_barrier(data_->list.Get(), record->resource.Get(), before, after);
    }
    record->state = after;
    use(*record);
    return core::success();
}

RhiStatus Dx12CommandList::transition(TextureHandle texture, ResourceState before, ResourceState after) {
    if (const RhiStatus open = require_recording("transition"); !open.ok()) {
        return open;
    }
    TextureObject* record = device_.find_texture(texture);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "transition: texture handle is invalid or destroyed");
    }
    if (record->state != before) {
        return fail(RhiErrorCode::StateMismatch, "transition: texture is not in the state the caller named as 'before'");
    }
    if (!texture_state_allowed(record->desc.usage, after)) {
        return fail(RhiErrorCode::InvalidDescriptor, "transition: the texture usage does not allow the target state");
    }
    if (before != after) {
        transition_barrier(data_->list.Get(), record->resource.Get(), before, after);
    }
    record->state = after;
    use(*record);
    return core::success();
}

// Compute, graphics, and mesh pipelines (US-029, US-033, US-038). A mesh pipeline has no input assembler, so it sets no topology.
RhiStatus Dx12CommandList::set_pipeline(PipelineHandle pipeline) {
    if (const RhiStatus open = require_recording("set_pipeline"); !open.ok()) {
        return open;
    }
    PipelineObject* object = device_.find_pipeline(pipeline);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_pipeline: pipeline handle is invalid or destroyed");
    }
    if (object->kind == PipelineKind::Compute && queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, "set_pipeline: compute pipelines cannot run on the copy queue");
    }
    if (object->kind != PipelineKind::Compute && queue_ != QueueType::Graphics) {
        return fail(RhiErrorCode::InvalidState, "set_pipeline: graphics and mesh pipelines need the graphics queue");
    }
    if (object->kind == PipelineKind::Compute) {
        data_->list->SetComputeRootSignature(object->root_signature.Get());
    } else {
        data_->list->SetGraphicsRootSignature(object->root_signature.Get());
        if (object->kind == PipelineKind::Graphics) {
            data_->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }
    }
    data_->list->SetPipelineState(object->state.Get());
    pipeline_ = share(object);
    ray_pipeline_.reset();
    // A new pipeline has its own layouts, so earlier set bindings no longer apply.
    bound_set_mask_ = 0;
    sets_.fill(nullptr);
    use(*object);
    return core::success();
}

RhiStatus Dx12CommandList::set_descriptor_set(std::uint32_t set_index, DescriptorSetHandle set) {
    if (const RhiStatus open = require_recording("set_descriptor_set"); !open.ok()) {
        return open;
    }
    if (!pipeline_ && !ray_pipeline_) {
        return fail(RhiErrorCode::InvalidState, "set_descriptor_set: bind a pipeline first");
    }
    const auto& layouts = active_layouts();
    if (set_index >= layouts.size()) {
        return fail(RhiErrorCode::OutOfRange, "set_descriptor_set: the pipeline has no descriptor set at this index");
    }
    DescriptorSetObject* object = device_.find_set(set);
    if (object == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_descriptor_set: descriptor set handle is invalid or destroyed");
    }
    if (object->bindings != layouts[set_index]) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_descriptor_set: the set's layout does not match the pipeline layout");
    }
    ID3D12DescriptorHeap* heaps[] = {device_.shader_heap().native()};
    data_->list->SetDescriptorHeaps(1, heaps);
    // Ray tracing pipelines use the compute root signature, so their tables are compute tables too.
    if (ray_pipeline_ || pipeline_->kind == PipelineKind::Compute) {
        data_->list->SetComputeRootDescriptorTable(set_index, device_.shader_heap().gpu(object->first_slot));
    } else {
        data_->list->SetGraphicsRootDescriptorTable(set_index, device_.shader_heap().gpu(object->first_slot));
    }
    sets_[set_index] = share(object);
    bound_set_mask_ |= 1u << set_index;
    use(*object);
    return core::success();
}

const std::vector<std::vector<DescriptorBindingDesc>>& Dx12CommandList::active_layouts() const {
    return ray_pipeline_ ? ray_pipeline_->layouts : pipeline_->layouts;
}

RhiStatus Dx12CommandList::check_bound_sets(std::string_view operation) {
    const std::string name(operation);
    const std::size_t count = active_layouts().size();
    const std::uint32_t wanted = count == 0 ? 0u : static_cast<std::uint32_t>((1ull << count) - 1ull);
    if ((bound_set_mask_ & wanted) != wanted) {
        return fail(RhiErrorCode::InvalidState, name + ": a descriptor set required by the pipeline is not bound");
    }
    for (std::size_t set_index = 0; set_index < count; ++set_index) {
        const DescriptorSetObject& set = *sets_[set_index];
        if (set.destroyed) {
            return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor set was destroyed");
        }
        for (const DescriptorElement& element : set.elements) {
            const ResourceState needed = descriptor_state(element.type);
            if (element.buffer) {
                if (element.buffer->destroyed) {
                    return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor refers to a destroyed resource");
                }
                if (element.buffer->state != needed) {
                    return fail(RhiErrorCode::StateMismatch, name + ": a bound resource is not in the state its descriptor needs");
                }
            } else if (element.texture) {
                if (element.texture->destroyed) {
                    return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor refers to a destroyed resource");
                }
                if (element.texture->state != needed) {
                    return fail(RhiErrorCode::StateMismatch, name + ": a bound resource is not in the state its descriptor needs");
                }
            } else if (element.acceleration) {
                // An acceleration structure has no tracked state; it must be alive and built.
                if (element.acceleration->destroyed) {
                    return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor refers to a destroyed acceleration structure");
                }
                if (!element.acceleration->built) {
                    return fail(RhiErrorCode::InvalidState, name + ": a bound acceleration structure has not been built");
                }
            } else {
                return fail(RhiErrorCode::InvalidState, name + ": a bound descriptor was never written");
            }
        }
    }
    // Every resource a bound descriptor names is held by this list until the submission completes.
    for (std::size_t set_index = 0; set_index < count; ++set_index) {
        for (const DescriptorElement& element : sets_[set_index]->elements) {
            if (element.buffer) {
                use(*element.buffer);
            } else if (element.texture) {
                use(*element.texture);
            } else {
                use(*element.acceleration);
            }
        }
    }
    return core::success();
}

RhiStatus Dx12CommandList::set_vertex_buffer(std::uint32_t slot, BufferHandle buffer) {
    if (const RhiStatus open = require_recording("set_vertex_buffer"); !open.ok()) {
        return open;
    }
    // Slot 0 is the per-vertex input and slot 1 the per-instance input (PipelineDesc::instance_attributes).
    if (slot > 1) {
        return fail(RhiErrorCode::OutOfRange, "set_vertex_buffer: only slots 0 and 1 are used by the vertex layout");
    }
    BufferObject* record = device_.find_buffer(buffer);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_vertex_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(record->desc.usage, BufferUsage::Vertex)) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_vertex_buffer: buffer was not created with Vertex usage");
    }
    (slot == 0 ? vertex_buffer_ : instance_buffer_) = share(record);
    use(*record);
    return core::success();
}

RhiStatus Dx12CommandList::set_index_buffer(BufferHandle buffer, IndexFormat format) {
    if (const RhiStatus open = require_recording("set_index_buffer"); !open.ok()) {
        return open;
    }
    BufferObject* record = device_.find_buffer(buffer);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_index_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(record->desc.usage, BufferUsage::Index)) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_index_buffer: buffer was not created with Index usage");
    }
    index_buffer_ = share(record);
    index_format_ = format;
    use(*record);
    return core::success();
}

RhiStatus Dx12CommandList::begin_render_pass(const RenderPassDesc& pass) {
    if (const RhiStatus open = require_recording("begin_render_pass"); !open.ok()) {
        return open;
    }
    if (queue_ != QueueType::Graphics) {
        return fail(RhiErrorCode::InvalidState, "begin_render_pass: render passes need the graphics queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "begin_render_pass: a render pass is already active");
    }
    if (pass.color_targets.empty() && !pass.depth_target) {
        return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: the pass needs at least one target");
    }
    if (pass.color_targets.size() > kMaxColorTargets) {
        return fail(RhiErrorCode::OutOfRange, "begin_render_pass: too many color targets");
    }

    std::vector<TextureObject*> colors;
    for (const TextureHandle& target : pass.color_targets) {
        TextureObject* record = device_.find_texture(target);
        if (record == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, "begin_render_pass: render target handle is invalid or destroyed");
        }
        if (!has_flags(record->desc.usage, TextureUsage::RenderTarget)) {
            return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: texture was not created with RenderTarget usage");
        }
        if (record->state != ResourceState::RenderTarget) {
            return fail(RhiErrorCode::StateMismatch, "begin_render_pass: render target must be in the RenderTarget state");
        }
        colors.push_back(record);
    }
    TextureObject* depth = nullptr;
    if (pass.depth_target) {
        depth = device_.find_texture(*pass.depth_target);
        if (depth == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, "begin_render_pass: depth target handle is invalid or destroyed");
        }
        if (!has_flags(depth->desc.usage, TextureUsage::DepthStencil)) {
            return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: texture was not created with DepthStencil usage");
        }
        if (depth->state != ResourceState::DepthWrite) {
            return fail(RhiErrorCode::StateMismatch, "begin_render_pass: depth target must be in the DepthWrite state");
        }
    }
    for (std::size_t i = 0; i < colors.size(); ++i) {
        for (std::size_t j = i + 1; j < colors.size(); ++j) {
            if (colors[i] == colors[j]) {
                return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: the pass names the same texture twice");
            }
        }
        if (colors[i] == depth) {
            return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: the pass names the same texture twice");
        }
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxColorTargets> rtvs{};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        rtvs[i] = colors[i]->rtv;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (depth != nullptr) {
        dsv = depth->dsv;
    }
    // The viewport and scissor cover the smallest target, as on Vulkan, where the render area is the same. The viewport is 0 to 1 deep.
    std::uint32_t width = UINT32_MAX;
    std::uint32_t height = UINT32_MAX;
    for (const TextureObject* target : colors) {
        width = std::min(width, target->desc.width);
        height = std::min(height, target->desc.height);
    }
    if (depth != nullptr) {
        width = std::min(width, depth->desc.width);
        height = std::min(height, depth->desc.height);
    }
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    data_->list->RSSetViewports(1, &viewport);
    data_->list->RSSetScissorRects(1, &scissor);
    data_->list->OMSetRenderTargets(static_cast<UINT>(colors.size()), rtvs.data(), FALSE, depth != nullptr ? &dsv : nullptr);
    if (pass.clear_color) {
        for (std::size_t i = 0; i < colors.size(); ++i) {
            data_->list->ClearRenderTargetView(rtvs[i], pass.clear_color->data(), 0, nullptr);
        }
    }
    if (pass.clear_depth && depth != nullptr) {
        data_->list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, *pass.clear_depth, 0, 0, nullptr);
    }
    for (TextureObject* color : colors) {
        use(*color);
    }
    if (depth != nullptr) {
        use(*depth);
    }
    pass_active_ = true;
    return core::success();
}

RhiStatus Dx12CommandList::end_render_pass() {
    if (const RhiStatus open = require_recording("end_render_pass"); !open.ok()) {
        return open;
    }
    if (!pass_active_) {
        return fail(RhiErrorCode::InvalidState, "end_render_pass: no render pass is active");
    }
    pass_active_ = false;
    return core::success();
}

RhiStatus Dx12CommandList::draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                                        std::int32_t base_vertex, std::uint32_t first_instance) {
    if (const RhiStatus open = require_recording("draw_indexed"); !open.ok()) {
        return open;
    }
    if (!pass_active_ || !pipeline_ || pipeline_->kind != PipelineKind::Graphics) {
        return fail(RhiErrorCode::InvalidState, "draw_indexed: needs an active render pass and a bound graphics pipeline");
    }
    if (!vertex_buffer_ || !index_buffer_) {
        return fail(RhiErrorCode::InvalidState, "draw_indexed: vertex and index buffers must be bound");
    }
    if (index_count == 0 || instance_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed: index and instance counts must be greater than zero");
    }
    const std::uint64_t index_bytes = index_stride_bytes(index_format_);
    if (vertex_buffer_->destroyed || index_buffer_->destroyed) {
        return fail(RhiErrorCode::InvalidHandle, "draw_indexed: a bound vertex or index buffer was destroyed");
    }
    if (vertex_buffer_->state != ResourceState::VertexBuffer || index_buffer_->state != ResourceState::IndexBuffer) {
        return fail(RhiErrorCode::StateMismatch, "draw_indexed: a bound vertex or index buffer is not in its input state");
    }
    if ((static_cast<std::uint64_t>(first_index) + index_count) * index_bytes > index_buffer_->desc.size_bytes) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed: the index range is outside the bound index buffer");
    }
    if (const RhiStatus bound = check_bound_sets("draw_indexed"); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind_input("draw_indexed"); !bound.ok()) {
        return bound;
    }
    data_->list->DrawIndexedInstanced(index_count, instance_count, first_index, base_vertex, first_instance);
    return core::success();
}

RhiStatus Dx12CommandList::bind_input(std::string_view operation) {
    const std::string name(operation);
    if (!vertex_buffer_ || !index_buffer_) {
        return fail(RhiErrorCode::InvalidState, name + ": vertex and index buffers must be bound");
    }
    const bool per_instance = pipeline_->instance_stride != 0;
    if (per_instance && !instance_buffer_) {
        return fail(RhiErrorCode::InvalidState, name + ": the pipeline reads per-instance input, so slot 1 must be bound");
    }
    std::array<D3D12_VERTEX_BUFFER_VIEW, 2> views{};
    views[0] = D3D12_VERTEX_BUFFER_VIEW{vertex_buffer_->resource->GetGPUVirtualAddress(), static_cast<UINT>(vertex_buffer_->desc.size_bytes),
                                        pipeline_->vertex_stride};
    if (per_instance) {
        views[1] = D3D12_VERTEX_BUFFER_VIEW{instance_buffer_->resource->GetGPUVirtualAddress(),
                                            static_cast<UINT>(instance_buffer_->desc.size_bytes), pipeline_->instance_stride};
    }
    data_->list->IASetVertexBuffers(0, per_instance ? 2u : 1u, views.data());
    const D3D12_INDEX_BUFFER_VIEW index_view{index_buffer_->resource->GetGPUVirtualAddress(),
                                             static_cast<UINT>(index_buffer_->desc.size_bytes),
                                             index_format_ == IndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
    data_->list->IASetIndexBuffer(&index_view);
    return core::success();
}

// ExecuteIndirect with the DrawIndexedInstanced signature: `count` is the number of records the GPU runs, read from `count_buffer` when one is
// given, and at most `max_draws`. DX12 clamps the count to max_draws itself, so a count that overflows the arguments draws only what fits.
RhiStatus Dx12CommandList::execute_indexed(BufferHandle args, std::uint64_t args_offset, BufferHandle count_buffer, std::uint64_t count_offset,
                                           std::uint32_t max_draws, std::string_view operation) {
    if (const RhiStatus open = require_recording(operation); !open.ok()) {
        return open;
    }
    if (!pass_active_ || !pipeline_ || pipeline_->kind != PipelineKind::Graphics) {
        return fail(RhiErrorCode::InvalidState, std::string(operation) + ": needs an active render pass and a bound graphics pipeline");
    }
    BufferObject* args_record = device_.find_buffer(args);
    if (args_record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, std::string(operation) + ": argument buffer handle is invalid or destroyed");
    }
    BufferObject* count_record = nullptr;
    if (count_buffer.valid()) {
        count_record = device_.find_buffer(count_buffer);
        if (count_record == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, std::string(operation) + ": count buffer handle is invalid or destroyed");
        }
    }
    if (const RhiStatus bound = check_bound_sets(operation); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind_input(operation); !bound.ok()) {
        return bound;
    }
    const RhiResult<ID3D12CommandSignature*> signature = device_.draw_indexed_signature();
    if (!signature.ok()) {
        return core::failure(signature.error());
    }
    use(*args_record);
    if (count_record != nullptr) {
        use(*count_record);
    }
    data_->list->ExecuteIndirect(signature.value(), max_draws, args_record->resource.Get(), args_offset,
                                 count_record != nullptr ? count_record->resource.Get() : nullptr, count_offset);
    return core::success();
}

RhiStatus Dx12CommandList::draw_indexed_indirect(BufferHandle args, std::uint64_t args_offset, std::uint32_t draw_count) {
    if (draw_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect: draw count must be greater than zero");
    }
    return execute_indexed(args, args_offset, BufferHandle{}, 0, draw_count, "draw_indexed_indirect");
}

RhiStatus Dx12CommandList::draw_indexed_indirect_count(BufferHandle args, std::uint64_t args_offset, BufferHandle count,
                                                       std::uint64_t count_offset, std::uint32_t max_draw_count) {
    if (max_draw_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect_count: max draw count must be greater than zero");
    }
    return execute_indexed(args, args_offset, count, count_offset, max_draw_count, "draw_indexed_indirect_count");
}

RhiStatus Dx12CommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (const RhiStatus open = require_recording("dispatch"); !open.ok()) {
        return open;
    }
    // D3D12 accepts at most 65535 groups per dimension.
    constexpr std::uint32_t kMaxGroups = 65535;
    if (x == 0 || y == 0 || z == 0) {
        return fail(RhiErrorCode::OutOfRange, "dispatch: group counts must be greater than zero");
    }
    if (x > kMaxGroups || y > kMaxGroups || z > kMaxGroups) {
        return fail(RhiErrorCode::OutOfRange, "dispatch: a group count exceeds 65535");
    }
    if (pipeline_ && pipeline_->kind != PipelineKind::Compute) {
        return fail(RhiErrorCode::InvalidState, "dispatch: needs a bound compute pipeline");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "dispatch: compute cannot run inside a render pass");
    }
    if (!pipeline_) {
        return fail(RhiErrorCode::InvalidState, "dispatch: bind a compute pipeline first");
    }
    if (const RhiStatus bound = check_bound_sets("dispatch"); !bound.ok()) {
        return bound;
    }
    data_->list->Dispatch(x, y, z);
    return core::success();
}

RhiStatus Dx12CommandList::dispatch_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (const RhiStatus open = require_recording("dispatch_mesh"); !open.ok()) {
        return open;
    }
    if (!pass_active_ || !pipeline_ || pipeline_->kind != PipelineKind::Mesh) {
        return fail(RhiErrorCode::InvalidState, "dispatch_mesh: needs an active render pass and a bound mesh pipeline");
    }
    if (x == 0 || y == 0 || z == 0) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_mesh: group counts must be greater than zero");
    }
    constexpr std::uint32_t kMaxMeshGroups = 65535;
    if (x > kMaxMeshGroups || y > kMaxMeshGroups || z > kMaxMeshGroups) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_mesh: a group count exceeds 65535");
    }
    if (const RhiStatus bound = check_bound_sets("dispatch_mesh"); !bound.ok()) {
        return bound;
    }
    // DispatchMesh is on ID3D12GraphicsCommandList6, which the list implements on every device with mesh shaders.
    ComPtr<ID3D12GraphicsCommandList6> mesh_list;
    if (const HRESULT hr = data_->list.As(&mesh_list); FAILED(hr)) {
        return core::failure(hresult_error("dispatch_mesh: ID3D12GraphicsCommandList6", hr));
    }
    mesh_list->DispatchMesh(x, y, z);
    return core::success();
}

// FSR 2 (US-046). The upscaler records its own barriers and binds its own pipelines and descriptor heaps, so the list forgets what it had
// bound: the caller binds again before the next dispatch. The inputs and the output keep the states they had, which the checks require.
RhiStatus Dx12CommandList::upscale(Upscaler& upscaler, const UpscaleDispatch& dispatch) {
    if (const RhiStatus open = require_recording("upscale"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "upscale: FSR 2 cannot run inside a render pass");
    }
#if !ENGINE_FSR2_AVAILABLE
    (void)upscaler;
    (void)dispatch;
    return fail(RhiErrorCode::Unsupported, "upscale: FSR 2 is not built; its shader permutation headers are not vendored");
#else
    auto* dx12_upscaler = dynamic_cast<Dx12Upscaler*>(&upscaler);
    if (dx12_upscaler == nullptr || dx12_upscaler->device_id() != device_.id()) {
        return fail(RhiErrorCode::InvalidHandle, "upscale: the upscaler belongs to another device or backend");
    }
    if (const RhiStatus valid = validate_upscale_dispatch(upscaler.desc(), dispatch); !valid.ok()) {
        return valid;
    }
    if (!dispatch.color.valid() || !dispatch.depth.valid() || !dispatch.motion.valid() || !dispatch.output.valid()) {
        return fail(RhiErrorCode::InvalidHandle, "upscale: color, depth, motion, and output are required");
    }
    UpscaleImages images;
    const auto resolve = [&](TextureHandle handle, UpscaleRole role, TextureObject*& out) -> RhiStatus {
        if (!handle.valid()) {
            return core::success();
        }
        TextureObject* record = device_.find_texture(handle);
        if (record == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, "upscale: a texture handle is invalid or destroyed");
        }
        const RhiStatus checked = check_upscale_texture(role, upscaler.desc(), dispatch, record->desc, record->state);
        if (checked.ok()) {
            out = record;
        }
        return checked;
    };
    if (RhiStatus status = resolve(dispatch.color, UpscaleRole::Color, images.color); !status.ok()) {
        return status;
    }
    if (RhiStatus status = resolve(dispatch.depth, UpscaleRole::Depth, images.depth); !status.ok()) {
        return status;
    }
    if (RhiStatus status = resolve(dispatch.motion, UpscaleRole::Motion, images.motion); !status.ok()) {
        return status;
    }
    if (RhiStatus status = resolve(dispatch.reactive, UpscaleRole::Reactive, images.reactive); !status.ok()) {
        return status;
    }
    if (RhiStatus status = resolve(dispatch.exposure, UpscaleRole::Exposure, images.exposure); !status.ok()) {
        return status;
    }
    if (RhiStatus status = resolve(dispatch.output, UpscaleRole::Output, images.output); !status.ok()) {
        return status;
    }

    if (RhiStatus status = dx12_upscaler->record(data_->list.Get(), images, dispatch); !status.ok()) {
        return status;
    }
    for (TextureObject* texture : {images.color, images.depth, images.motion, images.reactive, images.exposure, images.output}) {
        if (texture != nullptr) {
            use(*texture);
        }
    }
    // FSR 2 set its own root signature and descriptor heap. The shader heap is set again by the next set_descriptor_set.
    pipeline_.reset();
    ray_pipeline_.reset();
    sets_.fill(nullptr);
    bound_set_mask_ = 0;
    return core::success();
#endif
}

RhiStatus Dx12CommandList::copy_buffer(BufferHandle source, std::uint64_t source_offset, BufferHandle destination,
                                       std::uint64_t destination_offset, std::uint64_t size) {
    if (const RhiStatus open = require_recording("copy_buffer"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "copy_buffer: copies cannot run inside a render pass");
    }
    if (size == 0) {
        return fail(RhiErrorCode::OutOfRange, "copy_buffer: size must be greater than zero");
    }
    if (source == destination) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_buffer: source and destination must be different buffers");
    }
    BufferObject* src = device_.find_buffer(source);
    BufferObject* dst = device_.find_buffer(destination);
    if (src == nullptr || dst == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "copy_buffer: buffer handle is invalid or destroyed");
    }
    if (!has_flags(src->desc.usage, BufferUsage::CopySource) || src->state != ResourceState::CopySource) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer: source needs CopySource usage and state");
    }
    if (!has_flags(dst->desc.usage, BufferUsage::CopyDestination) || dst->state != ResourceState::CopyDestination) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer: destination needs CopyDestination usage and state");
    }
    if (source_offset > src->desc.size_bytes || size > src->desc.size_bytes - source_offset ||
        destination_offset > dst->desc.size_bytes || size > dst->desc.size_bytes - destination_offset) {
        return fail(RhiErrorCode::OutOfRange, "copy_buffer: range is outside a buffer");
    }
    data_->list->CopyBufferRegion(dst->resource.Get(), destination_offset, src->resource.Get(), source_offset, size);
    use(*src);
    use(*dst);
    return core::success();
}

RhiStatus Dx12CommandList::copy_texture_to_buffer(TextureHandle source, BufferHandle destination, std::uint64_t destination_offset) {
    if (const RhiStatus open = require_recording("copy_texture_to_buffer"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "copy_texture_to_buffer: copies cannot run inside a render pass");
    }
    TextureObject* src = device_.find_texture(source);
    BufferObject* dst = device_.find_buffer(destination);
    if (src == nullptr || dst == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "copy_texture_to_buffer: handle is invalid or destroyed");
    }
    if (bytes_per_pixel(src->desc.format) == 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_texture_to_buffer: the texture format has no copyable color texels");
    }
    if (!has_flags(src->desc.usage, TextureUsage::CopySource) || src->state != ResourceState::CopySource) {
        return fail(RhiErrorCode::StateMismatch, "copy_texture_to_buffer: source needs CopySource usage and state");
    }
    if (!has_flags(dst->desc.usage, BufferUsage::CopyDestination) || dst->state != ResourceState::CopyDestination) {
        return fail(RhiErrorCode::StateMismatch, "copy_texture_to_buffer: destination needs CopyDestination usage and state");
    }
    if (destination_offset % kTexturePlacementAlignment != 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_texture_to_buffer: destination offset must be a multiple of 512 bytes");
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(src->desc.width) * bytes_per_pixel(src->desc.format);
    const std::uint64_t footprint = copy_row_pitch(src->desc.width, src->desc.format) * (src->desc.height - 1) + row_bytes;
    if (destination_offset > dst->desc.size_bytes || footprint > dst->desc.size_bytes - destination_offset) {
        return fail(RhiErrorCode::OutOfRange, "copy_texture_to_buffer: the footprint is outside the destination buffer");
    }

    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = dst->resource.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Offset = destination_offset;
    target.PlacedFootprint.Footprint.Format = to_dxgi(src->desc.format);
    target.PlacedFootprint.Footprint.Width = src->desc.width;
    target.PlacedFootprint.Footprint.Height = src->desc.height;
    target.PlacedFootprint.Footprint.Depth = 1;
    target.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(copy_row_pitch(src->desc.width, src->desc.format));

    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = src->resource.Get();
    origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    origin.SubresourceIndex = 0;

    data_->list->CopyTextureRegion(&target, 0, 0, 0, &origin, nullptr);
    use(*src);
    use(*dst);
    return core::success();
}

RhiStatus Dx12CommandList::copy_buffer_to_texture(BufferHandle source, std::uint64_t source_offset, TextureHandle destination) {
    if (const RhiStatus open = require_recording("copy_buffer_to_texture"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "copy_buffer_to_texture: copies cannot run inside a render pass");
    }
    BufferObject* src = device_.find_buffer(source);
    TextureObject* dst = device_.find_texture(destination);
    if (src == nullptr || dst == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "copy_buffer_to_texture: handle is invalid or destroyed");
    }
    if (bytes_per_pixel(dst->desc.format) == 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_buffer_to_texture: the texture format has no copyable color texels");
    }
    if (!has_flags(src->desc.usage, BufferUsage::CopySource) || src->state != ResourceState::CopySource) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer_to_texture: source needs CopySource usage and state");
    }
    if (!has_flags(dst->desc.usage, TextureUsage::CopyDestination) || dst->state != ResourceState::CopyDestination) {
        return fail(RhiErrorCode::StateMismatch, "copy_buffer_to_texture: destination needs CopyDestination usage and state");
    }
    if (source_offset % kTexturePlacementAlignment != 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "copy_buffer_to_texture: source offset must be a multiple of 512 bytes");
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(dst->desc.width) * bytes_per_pixel(dst->desc.format);
    const std::uint64_t footprint = copy_row_pitch(dst->desc.width, dst->desc.format) * (dst->desc.height - 1) + row_bytes;
    if (source_offset > src->desc.size_bytes || footprint > src->desc.size_bytes - source_offset) {
        return fail(RhiErrorCode::OutOfRange, "copy_buffer_to_texture: the footprint is outside the source buffer");
    }

    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = dst->resource.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = src->resource.Get();
    origin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    origin.PlacedFootprint.Offset = source_offset;
    origin.PlacedFootprint.Footprint.Format = to_dxgi(dst->desc.format);
    origin.PlacedFootprint.Footprint.Width = dst->desc.width;
    origin.PlacedFootprint.Footprint.Height = dst->desc.height;
    origin.PlacedFootprint.Footprint.Depth = 1;
    origin.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(copy_row_pitch(dst->desc.width, dst->desc.format));

    data_->list->CopyTextureRegion(&target, 0, 0, 0, &origin, nullptr);
    use(*src);
    use(*dst);
    return core::success();
}

RhiStatus Dx12CommandList::write_timestamp(QueryPoolHandle pool, std::uint32_t index) {
    if (const RhiStatus open = require_recording("write_timestamp"); !open.ok()) {
        return open;
    }
    if (!device_.capabilities().enabled(Feature::TimestampQueries)) {
        return fail(RhiErrorCode::FeatureDisabled, "write_timestamp: the TimestampQueries feature is not enabled");
    }
    QueryPoolObject* query = device_.find_query_pool(pool);
    if (query == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_timestamp: query pool handle is invalid or destroyed");
    }
    if (index >= query->count) {
        return fail(RhiErrorCode::OutOfRange, "write_timestamp: index is outside the query pool");
    }
    // The result is resolved into the pool's readback buffer by the same submission, so read_timestamp needs no extra copy.
    data_->list->EndQuery(query->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    data_->list->ResolveQueryData(query->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index, 1, query->readback.Get(),
                                  static_cast<UINT64>(index) * sizeof(std::uint64_t));
    use(*query);
    timestamps_.emplace_back(std::static_pointer_cast<QueryPoolObject>(query->shared_from_this()), index);
    return core::success();
}

RhiStatus Dx12CommandList::close() {
    if (const RhiStatus open = require_recording("close"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "close: a render pass is still active");
    }
    const HRESULT hr = data_->list->Close();
    if (FAILED(hr)) {
        return core::failure(hresult_error("close", hr));
    }
    state_ = ListState::Closed;
    return core::success();
}

}  // namespace engine::rhi::dx12
