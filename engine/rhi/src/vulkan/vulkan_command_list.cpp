#include "vulkan/vulkan_command_list.hpp"

#include "fsr2_common.hpp"
#if ENGINE_FSR2_AVAILABLE
#include "vulkan/vulkan_upscaler.hpp"
#endif

#include "vulkan/vulkan_common.hpp"

#include "engine/core/result.hpp"

#include <algorithm>
#include <array>

namespace engine::rhi::vulkan {
namespace {

// Compute and graphics pipelines bind to different points, and descriptor sets follow the pipeline they are bound with.
// Graphics and mesh pipelines bind at the graphics point; only compute pipelines bind at the compute point (US-038 mesh pipelines were
// mapped to compute, which the driver rejects by crashing).
VkPipelineBindPoint bind_point(PipelineKind kind) noexcept {
    return kind == PipelineKind::Compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
}

// A barrier may name only the stages its queue family runs. Shader and draw stages do not exist on the copy queue, and graphics-only
// stages do not exist on the compute queue. Such a barrier widens to ALL_COMMANDS, which every queue family accepts.
VkPipelineStageFlags2 queue_stages(VkPipelineStageFlags2 stages, QueueType queue) noexcept {
    constexpr VkPipelineStageFlags2 kOrdering = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT |
                                                VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    constexpr VkPipelineStageFlags2 kComputeStages = kOrdering | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    switch (queue) {
        case QueueType::Graphics:
            return stages;
        case QueueType::Compute:
            return (stages & ~kComputeStages) == 0 ? stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        case QueueType::Copy:
            return (stages & ~kOrdering) == 0 ? stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

// Records the barrier that moves a buffer from `before` to `after` on `queue` (synchronization2).
void record_barrier(const Api& api, VkCommandBuffer command, QueueType queue, BufferObject& buffer, ResourceState before,
                    ResourceState after) {
    const SyncScope from = sync_for(before);
    const SyncScope to = sync_for(after);
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = queue_stages(from.stages, queue);
    barrier.srcAccessMask = from.access;
    barrier.dstStageMask = queue_stages(to.stages, queue);
    barrier.dstAccessMask = to.access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer.buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    api.vkCmdPipelineBarrier2KHR(command, &dependency);
}

// Records the barrier that moves a texture from `before` to `after` on `queue`, and moves the layout the device tracks with it
// (synchronization2).
void record_barrier(const Api& api, VkCommandBuffer command, QueueType queue, TextureObject& texture, ResourceState before,
                    ResourceState after) {
    const SyncScope from = sync_for(before);
    const SyncScope to = sync_for(after);
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = queue_stages(from.stages, queue);
    barrier.srcAccessMask = from.access;
    barrier.dstStageMask = queue_stages(to.stages, queue);
    barrier.dstAccessMask = to.access;
    barrier.oldLayout = texture.layout;
    barrier.newLayout = layout_for(after);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = aspect_for(texture.desc.format);
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = texture.desc.mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    api.vkCmdPipelineBarrier2KHR(command, &dependency);
    texture.layout = barrier.newLayout;
}

// The state a descriptor of this type needs its resource in while a dispatch runs.
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

VulkanCommandList::VulkanCommandList(VulkanDevice& device, QueueType queue_type, std::shared_ptr<ListData> data)
    : device_(device), queue_(queue_type), data_(std::move(data)) {}

QueueType VulkanCommandList::queue() const noexcept {
    return queue_;
}

RhiStatus VulkanCommandList::require_recording(std::string_view operation) const {
    if (state_ != ListState::Recording) {
        return fail(RhiErrorCode::InvalidState,
                    std::string(operation) + ": the command list is not recording; it was closed or already submitted");
    }
    return core::success();
}

void VulkanCommandList::use(ObjectBase& object) {
    refs_.push_back(object.shared_from_this());
}

RhiStatus VulkanCommandList::transition(BufferHandle buffer, ResourceState before, ResourceState after) {
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
        record_barrier(device_.api(), data_->buffer, queue_, *record, before, after);
    }
    record->state = after;
    use(*record);
    return core::success();
}

RhiStatus VulkanCommandList::transition(TextureHandle texture, ResourceState before, ResourceState after) {
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
        record_barrier(device_.api(), data_->buffer, queue_, *record, before, after);
    }
    record->state = after;
    use(*record);
    return core::success();
}

RhiStatus VulkanCommandList::set_pipeline(PipelineHandle pipeline) {
    if (const RhiStatus open = require_recording("set_pipeline"); !open.ok()) {
        return open;
    }
    PipelineObject* record = device_.find_pipeline(pipeline);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_pipeline: pipeline handle is invalid or destroyed");
    }
    if (record->kind == PipelineKind::Compute && queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, "set_pipeline: compute pipelines need the graphics or compute queue");
    }
    if (record->kind != PipelineKind::Compute && queue_ != QueueType::Graphics) {
        return fail(RhiErrorCode::InvalidState, "set_pipeline: graphics and mesh pipelines need the graphics queue");
    }
    device_.api().vkCmdBindPipeline(data_->buffer, bind_point(record->kind), record->pipeline);
    ray_pipeline_.reset();
    pipeline_ = std::static_pointer_cast<PipelineObject>(record->shared_from_this());
    // A new pipeline has its own layout, so the sets bound for the earlier one no longer apply.
    sets_.fill(nullptr);
    bound_set_mask_ = 0;
    use(*record);
    return core::success();
}

RhiStatus VulkanCommandList::set_descriptor_set(std::uint32_t set_index, DescriptorSetHandle set) {
    if (const RhiStatus open = require_recording("set_descriptor_set"); !open.ok()) {
        return open;
    }
    // The bound pipeline supplies the layouts and the bind point: a compute or graphics pipeline, or a ray tracing pipeline (US-042).
    const std::vector<std::vector<DescriptorBindingDesc>>* layouts = nullptr;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_COMPUTE;
    if (pipeline_) {
        layouts = &pipeline_->layouts;
        pipeline_layout = pipeline_->layout;
        point = bind_point(pipeline_->kind);
    } else if (ray_pipeline_) {
        layouts = &ray_pipeline_->layouts;
        pipeline_layout = ray_pipeline_->layout;
        point = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
    } else {
        return fail(RhiErrorCode::InvalidState, "set_descriptor_set: bind a pipeline first");
    }
    if (set_index >= layouts->size()) {
        return fail(RhiErrorCode::OutOfRange, "set_descriptor_set: the pipeline has no descriptor set at this index");
    }
    DescriptorSetObject* record = device_.find_descriptor_set(set);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_descriptor_set: descriptor set handle is invalid or destroyed");
    }
    if (record->bindings != (*layouts)[set_index]) {
        return fail(RhiErrorCode::InvalidDescriptor, "set_descriptor_set: the set's layout does not match the pipeline layout");
    }
    device_.api().vkCmdBindDescriptorSets(data_->buffer, point, pipeline_layout, set_index, 1, &record->set, 0, nullptr);
    sets_[set_index] = std::static_pointer_cast<DescriptorSetObject>(record->shared_from_this());
    bound_set_mask_ |= 1u << set_index;
    use(*record);
    return core::success();
}

RhiStatus VulkanCommandList::set_vertex_buffer(std::uint32_t slot, BufferHandle buffer) {
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
    const VkBuffer native = record->buffer;
    const VkDeviceSize offset = 0;
    device_.api().vkCmdBindVertexBuffers(data_->buffer, slot, 1, &native, &offset);
    (slot == 0 ? vertex_buffer_ : instance_buffer_) = std::static_pointer_cast<BufferObject>(record->shared_from_this());
    use(*record);
    return core::success();
}

RhiStatus VulkanCommandList::set_index_buffer(BufferHandle buffer, IndexFormat format) {
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
    const VkIndexType type = format == IndexFormat::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    device_.api().vkCmdBindIndexBuffer(data_->buffer, record->buffer, 0, type);
    index_buffer_ = std::static_pointer_cast<BufferObject>(record->shared_from_this());
    index_format_ = format;
    use(*record);
    return core::success();
}

RhiStatus VulkanCommandList::begin_render_pass(const RenderPassDesc& pass) {
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

    std::vector<TextureObject*> targets;
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_refs;
    std::vector<VkImageView> views;
    std::vector<VkClearValue> clears;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (std::size_t i = 0; i < pass.color_targets.size(); ++i) {
        TextureObject* record = device_.find_texture(pass.color_targets[i]);
        if (record == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, "begin_render_pass: render target handle is invalid or destroyed");
        }
        if (!has_flags(record->desc.usage, TextureUsage::RenderTarget) || is_depth_format(record->desc.format)) {
            return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: texture was not created as a color RenderTarget");
        }
        if (record->state != ResourceState::RenderTarget) {
            return fail(RhiErrorCode::StateMismatch, "begin_render_pass: render target must be in the RenderTarget state");
        }
        VkAttachmentDescription attachment{};
        attachment.format = to_vk(record->desc.format);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = pass.clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(attachment);
        color_refs.push_back(VkAttachmentReference{static_cast<std::uint32_t>(i), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        views.push_back(record->view);
        VkClearValue clear{};
        if (pass.clear_color) {
            clear.color.float32[0] = (*pass.clear_color)[0];
            clear.color.float32[1] = (*pass.clear_color)[1];
            clear.color.float32[2] = (*pass.clear_color)[2];
            clear.color.float32[3] = (*pass.clear_color)[3];
        }
        clears.push_back(clear);
        targets.push_back(record);
    }
    VkAttachmentReference depth_ref{};
    bool has_depth = false;
    if (pass.depth_target) {
        TextureObject* depth = device_.find_texture(*pass.depth_target);
        if (depth == nullptr) {
            return fail(RhiErrorCode::InvalidHandle, "begin_render_pass: depth target handle is invalid or destroyed");
        }
        if (!has_flags(depth->desc.usage, TextureUsage::DepthStencil) || !is_depth_format(depth->desc.format)) {
            return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: texture was not created as a DepthStencil target");
        }
        if (depth->state != ResourceState::DepthWrite) {
            return fail(RhiErrorCode::StateMismatch, "begin_render_pass: depth target must be in the DepthWrite state");
        }
        VkAttachmentDescription attachment{};
        attachment.format = to_vk(depth->desc.format);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachment.loadOp = pass.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;  // else the depth contents are kept
        depth_ref = VkAttachmentReference{static_cast<std::uint32_t>(attachments.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        attachments.push_back(attachment);
        views.push_back(depth->view);
        VkClearValue depth_clear{};
        if (pass.clear_depth) {
            depth_clear.depthStencil.depth = *pass.clear_depth;
        }
        clears.push_back(depth_clear);
        targets.push_back(depth);
        has_depth = true;
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        for (std::size_t j = i + 1; j < targets.size(); ++j) {
            if (targets[i] == targets[j]) {
                return fail(RhiErrorCode::InvalidDescriptor, "begin_render_pass: the pass names the same texture twice");
            }
        }
    }
    for (TextureObject* target : targets) {
        const TextureDesc& desc = target->desc;
        width = width == 0 ? desc.width : std::min(width, desc.width);
        height = height == 0 ? desc.height : std::min(height, desc.height);
    }

    // The pass is created for this list alone. The device keeps the list, and so the pass, until the submission completes.
    const Api& api = device_.api();
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<std::uint32_t>(color_refs.size());
    subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

    // Earlier writes to these attachments, from this list or an earlier one, must finish before this pass touches them again.
    const VkSubpassDependency dependency = attachment_dependency(has_depth);

    VkRenderPassCreateInfo pass_info{};

    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass_info.pAttachments = attachments.data();
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dependency;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (const VkResult result = api.vkCreateRenderPass(device_.ref().device, &pass_info, nullptr, &render_pass); result != VK_SUCCESS) {
        return core::failure(vk_error("begin_render_pass: vkCreateRenderPass", result));
    }
    data_->render_passes.push_back(render_pass);

    VkFramebufferCreateInfo framebuffer_info{};

    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebuffer_info.pAttachments = views.data();
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (const VkResult result = api.vkCreateFramebuffer(device_.ref().device, &framebuffer_info, nullptr, &framebuffer);
        result != VK_SUCCESS) {
        return core::failure(vk_error("begin_render_pass: vkCreateFramebuffer", result));
    }
    data_->framebuffers.push_back(framebuffer);

    VkRenderPassBeginInfo begin{};

    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.offset = VkOffset2D{0, 0};
    begin.renderArea.extent = VkExtent2D{width, height};
    begin.clearValueCount = static_cast<std::uint32_t>(clears.size());
    begin.pClearValues = clears.data();
    api.vkCmdBeginRenderPass(data_->buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    // The viewport and scissor cover the render area. Vulkan's NDC y points down; see coords::to_vulkan_clip for the shader side.
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const VkRect2D scissor{VkOffset2D{0, 0}, VkExtent2D{width, height}};
    api.vkCmdSetViewport(data_->buffer, 0, 1, &viewport);
    api.vkCmdSetScissor(data_->buffer, 0, 1, &scissor);

    for (TextureObject* target : targets) {
        use(*target);
    }
    pass_active_ = true;
    return core::success();
}

RhiStatus VulkanCommandList::end_render_pass() {
    if (const RhiStatus open = require_recording("end_render_pass"); !open.ok()) {
        return open;
    }
    if (!pass_active_) {
        return fail(RhiErrorCode::InvalidState, "end_render_pass: no render pass is active");
    }
    device_.api().vkCmdEndRenderPass(data_->buffer);
    pass_active_ = false;
    return core::success();
}

RhiStatus VulkanCommandList::draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
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
    if (pipeline_->instance_stride != 0 && !instance_buffer_) {
        return fail(RhiErrorCode::InvalidState, "draw_indexed: the pipeline reads per-instance input, so slot 1 must be bound");
    }
    if (index_count == 0 || instance_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed: index and instance counts must be greater than zero");
    }
    if (vertex_buffer_->destroyed || index_buffer_->destroyed) {
        return fail(RhiErrorCode::InvalidHandle, "draw_indexed: a bound vertex or index buffer was destroyed");
    }
    if (vertex_buffer_->state != ResourceState::VertexBuffer || index_buffer_->state != ResourceState::IndexBuffer) {
        return fail(RhiErrorCode::StateMismatch, "draw_indexed: a bound vertex or index buffer is not in its input state");
    }
    if ((static_cast<std::uint64_t>(first_index) + index_count) * index_stride_bytes(index_format_) > index_buffer_->desc.size_bytes) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed: the index range is outside the bound index buffer");
    }
    if (const RhiStatus ready = check_compute_state("draw_indexed"); !ready.ok()) {
        return ready;
    }
    device_.api().vkCmdDrawIndexed(data_->buffer, index_count, instance_count, first_index, base_vertex, first_instance);
    return core::success();
}

// Checks what an indexed indirect draw needs from its bound state, the same as draw_indexed does for a direct one.
RhiStatus VulkanCommandList::check_indexed_indirect(std::string_view operation) {
    const std::string name(operation);
    if (!pass_active_ || !pipeline_ || pipeline_->kind != PipelineKind::Graphics) {
        return fail(RhiErrorCode::InvalidState, name + ": needs an active render pass and a bound graphics pipeline");
    }
    if (!vertex_buffer_ || !index_buffer_) {
        return fail(RhiErrorCode::InvalidState, name + ": vertex and index buffers must be bound");
    }
    if (pipeline_->instance_stride != 0 && !instance_buffer_) {
        return fail(RhiErrorCode::InvalidState, name + ": the pipeline reads per-instance input, so slot 1 must be bound");
    }
    if (vertex_buffer_->destroyed || index_buffer_->destroyed || (instance_buffer_ && instance_buffer_->destroyed)) {
        return fail(RhiErrorCode::InvalidHandle, name + ": a bound vertex or index buffer was destroyed");
    }
    if (vertex_buffer_->state != ResourceState::VertexBuffer || index_buffer_->state != ResourceState::IndexBuffer ||
        (instance_buffer_ && instance_buffer_->state != ResourceState::VertexBuffer)) {
        return fail(RhiErrorCode::StateMismatch, name + ": a bound vertex or index buffer is not in its input state");
    }
    return check_compute_state(operation);
}

RhiStatus VulkanCommandList::draw_indexed_indirect(BufferHandle args, std::uint64_t args_offset, std::uint32_t draw_count) {
    if (const RhiStatus open = require_recording("draw_indexed_indirect"); !open.ok()) {
        return open;
    }
    if (draw_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect: draw count must be greater than zero");
    }
    BufferObject* args_record = device_.find_buffer(args);
    if (args_record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "draw_indexed_indirect: argument buffer handle is invalid or destroyed");
    }
    if (const RhiStatus ready = check_indexed_indirect("draw_indexed_indirect"); !ready.ok()) {
        return ready;
    }
    device_.api().vkCmdDrawIndexedIndirect(data_->buffer, args_record->buffer, args_offset, draw_count,
                                           static_cast<std::uint32_t>(kDrawIndexedIndirectStride));
    use(*args_record);
    return core::success();
}

RhiStatus VulkanCommandList::draw_indexed_indirect_count(BufferHandle args, std::uint64_t args_offset, BufferHandle count,
                                                         std::uint64_t count_offset, std::uint32_t max_draw_count) {
    if (const RhiStatus open = require_recording("draw_indexed_indirect_count"); !open.ok()) {
        return open;
    }
    if (max_draw_count == 0) {
        return fail(RhiErrorCode::OutOfRange, "draw_indexed_indirect_count: max draw count must be greater than zero");
    }
    BufferObject* args_record = device_.find_buffer(args);
    BufferObject* count_record = device_.find_buffer(count);
    if (args_record == nullptr || count_record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "draw_indexed_indirect_count: argument or count buffer handle is invalid or destroyed");
    }
    if (const RhiStatus ready = check_indexed_indirect("draw_indexed_indirect_count"); !ready.ok()) {
        return ready;
    }
    device_.api().vkCmdDrawIndexedIndirectCount(data_->buffer, args_record->buffer, args_offset, count_record->buffer, count_offset, max_draw_count,
                                                static_cast<std::uint32_t>(kDrawIndexedIndirectStride));
    use(*args_record);
    use(*count_record);
    return core::success();
}

RhiStatus VulkanCommandList::check_compute_state(std::string_view operation) {
    const std::string name(operation);
    if (pipeline_ && pipeline_->kind != PipelineKind::Compute) {
        // Draws and mesh dispatches share this path: they need the pass, the sets, and the states, and they reach the same checks below.
        if (!pass_active_) {
            return fail(RhiErrorCode::InvalidState, name + ": draws need an active render pass");
        }
    } else {
        if (queue_ == QueueType::Copy) {
            return fail(RhiErrorCode::InvalidState, name + ": compute work needs the graphics or compute queue");
        }
        if (pass_active_) {
            return fail(RhiErrorCode::InvalidState, name + ": compute work cannot run inside a render pass");
        }
    }
    if (!pipeline_) {
        return fail(RhiErrorCode::InvalidState, name + ": needs a bound compute pipeline");
    }
    return check_bound_sets(operation, pipeline_->layouts);
}

RhiStatus VulkanCommandList::check_bound_sets(std::string_view operation, const std::vector<std::vector<DescriptorBindingDesc>>& layouts) {
    const std::string name(operation);
    const std::size_t count = layouts.size();
    const std::uint32_t wanted = count == 0 ? 0u : static_cast<std::uint32_t>((1ull << count) - 1ull);
    if ((bound_set_mask_ & wanted) != wanted) {
        return fail(RhiErrorCode::InvalidState, name + ": a descriptor set required by the pipeline is not bound");
    }
    for (std::size_t set_index = 0; set_index < count; ++set_index) {
        const DescriptorSetObject& set = *sets_[set_index];
        if (set.destroyed) {
            return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor set was destroyed");
        }
        for (std::size_t position = 0; position < set.bindings.size(); ++position) {
            const DescriptorBindingDesc& binding = set.bindings[position];
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const DescriptorEntry& entry = set.entries[set.first_entry[position] + element];
                if (!entry.written) {
                    return fail(RhiErrorCode::InvalidState, name + ": a bound descriptor was never written");
                }
                if (entry.is_structure) {
                    // A top-level structure is read by the trace, so it must be built; its state is not tracked.
                    const AccelerationStructureObject* structure = device_.find_acceleration(entry.structure);
                    if (structure == nullptr) {
                        return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor refers to a destroyed structure");
                    }
                    if (!structure->built) {
                        return fail(RhiErrorCode::InvalidState, name + ": a bound structure has not been built");
                    }
                    continue;
                }
                ResourceState actual = ResourceState::Common;
                if (entry.is_texture) {
                    const TextureObject* texture = device_.find_texture(entry.image);
                    if (texture == nullptr) {
                        return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor refers to a destroyed texture");
                    }
                    actual = texture->state;
                } else {
                    const BufferObject* buffer = device_.find_buffer(entry.buffer);
                    if (buffer == nullptr) {
                        return fail(RhiErrorCode::InvalidHandle, name + ": a bound descriptor refers to a destroyed buffer");
                    }
                    actual = buffer->state;
                }
                if (actual != descriptor_state(entry.type)) {
                    return fail(RhiErrorCode::StateMismatch, name + ": a bound resource is not in the state its descriptor needs");
                }
            }
        }
    }

    // Every check passed, so the list now depends on each bound object until its submission completes.
    for (std::size_t set_index = 0; set_index < count; ++set_index) {
        const DescriptorSetObject& set = *sets_[set_index];
        use(*sets_[set_index]);
        for (const DescriptorEntry& entry : set.entries) {
            if (entry.is_structure) {
                use(*device_.find_acceleration(entry.structure));
            } else if (entry.is_texture) {
                use(*device_.find_texture(entry.image));
            } else {
                use(*device_.find_buffer(entry.buffer));
            }
        }
    }
    return core::success();
}

RhiStatus VulkanCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (const RhiStatus open = require_recording("dispatch"); !open.ok()) {
        return open;
    }
    if (x == 0 || y == 0 || z == 0) {
        return fail(RhiErrorCode::OutOfRange, "dispatch: group counts must be greater than zero");
    }
    if (pipeline_ && pipeline_->kind != PipelineKind::Compute) {
        return fail(RhiErrorCode::InvalidState, "dispatch: needs a bound compute pipeline");
    }
    if (const RhiStatus ready = check_compute_state("dispatch"); !ready.ok()) {
        return ready;
    }
    device_.api().vkCmdDispatch(data_->buffer, x, y, z);
    return core::success();
}

// FSR 2 (US-046). The upscaler records its own barriers and binds its own pipelines and sets, so the list forgets what it had bound: the
// caller binds again before the next dispatch. The inputs and the output keep the states they had, which the checks below require.
RhiStatus VulkanCommandList::upscale(Upscaler& upscaler, const UpscaleDispatch& dispatch) {
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
    auto* vulkan_upscaler = dynamic_cast<VulkanUpscaler*>(&upscaler);
    if (vulkan_upscaler == nullptr || vulkan_upscaler->device_id() != device_.id()) {
        return fail(RhiErrorCode::InvalidHandle, "upscale: the upscaler belongs to another device or backend");
    }
    if (const RhiStatus valid = validate_upscale_dispatch(upscaler.desc(), dispatch); !valid.ok()) {
        return valid;
    }
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
    UpscaleImages images;
    if (!dispatch.color.valid() || !dispatch.depth.valid() || !dispatch.motion.valid() || !dispatch.output.valid()) {
        return fail(RhiErrorCode::InvalidHandle, "upscale: color, depth, motion, and output are required");
    }
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

    if (RhiStatus status = vulkan_upscaler->record(data_->buffer, images, dispatch); !status.ok()) {
        return status;
    }
    for (TextureObject* texture : {images.color, images.depth, images.motion, images.reactive, images.exposure, images.output}) {
        if (texture != nullptr) {
            use(*texture);
        }
    }
    pipeline_.reset();
    ray_pipeline_.reset();
    sets_.fill(nullptr);
    bound_set_mask_ = 0;
    return core::success();
#endif
}

// Ray tracing (US-042). Builds, traces, and the pipeline binding follow the DX12 contract. The barriers below order every build after the
// writes it reads and every later read after the build that wrote the structure.
namespace {

// Orders every earlier write before a build reads its inputs or uses its scratch.
void record_build_barrier(const Api& api, VkCommandBuffer command, QueueType queue) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = queue_stages(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queue);
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = queue_stages(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, queue);
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    api.vkCmdPipelineBarrier2KHR(command, &dependency);
}

// Orders a build's writes before any later build, trace, or shader read of the structure, and its scratch before the next build.
void record_structure_barrier(const Api& api, VkCommandBuffer command, QueueType queue) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = queue_stages(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, queue);
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = queue_stages(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                        queue);
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                            VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    api.vkCmdPipelineBarrier2KHR(command, &dependency);
}

// Records the build of one structure. The scratch address is checked first, so a rejected build records nothing.
RhiStatus record_build(VulkanDevice& device, VkCommandBuffer command, QueueType queue, AccelerationStructureObject& object, bool refit,
                       std::uint32_t primitives, const char* operation) {
    const VkDeviceAddress scratch = device.buffer_address(object.scratch->buffer);
    const std::uint32_t alignment = device.scratch_alignment();
    if (alignment != 0 && scratch % alignment != 0) {
        return fail(RhiErrorCode::InvalidState, std::string(operation) + ": the scratch buffer is not aligned for the driver");
    }
    record_build_barrier(device.api(), command, queue);
    VkAccelerationStructureBuildGeometryInfoKHR info = acceleration_inputs(object, refit);
    info.scratchData.deviceAddress = scratch;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitives;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};
    device.api().vkCmdBuildAccelerationStructuresKHR(command, 1, &info, ranges);
    record_structure_barrier(device.api(), command, queue);
    return core::success();
}

}  // namespace

RhiStatus VulkanCommandList::build_bottom_level(AccelerationStructureHandle structure, BuildMode mode) {
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
    // The geometry's buffers are read during the build, so they must be alive and in the ShaderRead state.
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
    if (const RhiStatus recorded = record_build(device_, data_->buffer, queue_, *object, refit, object->primitive_count, "build_bottom_level");
        !recorded.ok()) {
        return recorded;
    }

    object->built = true;
    use(*object);
    use(*object->vertex_buffer);
    if (object->index_buffer) {
        use(*object->index_buffer);
    }
    return core::success();
}

RhiStatus VulkanCommandList::build_top_level(AccelerationStructureHandle structure, BuildMode mode) {
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
    if (const RhiStatus recorded = record_build(device_, data_->buffer, queue_, *object, refit, object->instance_count, "build_top_level");
        !recorded.ok()) {
        return recorded;
    }

    object->built = true;
    object->built_instances = object->instance_count;
    use(*object);
    // The list names every bottom-level structure the build reads, so a destroyed one is rejected at submit, not read.
    for (const auto& reference : object->references) {
        use(*reference);
    }
    return core::success();
}

RhiStatus VulkanCommandList::set_ray_tracing_pipeline(RayTracingPipelineHandle pipeline) {
    if (const RhiStatus open = require_recording("set_ray_tracing_pipeline"); !open.ok()) {
        return open;
    }
    if (queue_ == QueueType::Copy) {
        return fail(RhiErrorCode::InvalidState, "set_ray_tracing_pipeline: needs the graphics or compute queue");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "set_ray_tracing_pipeline: a render pass is active");
    }
    RayTracingPipelineObject* record = device_.find_ray_pipeline(pipeline);
    if (record == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "set_ray_tracing_pipeline: pipeline handle is invalid or destroyed");
    }
    device_.api().vkCmdBindPipeline(data_->buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, record->pipeline);
    // The ray tracing pipeline replaces any graphics or compute pipeline, and sets bound for an earlier pipeline do not carry over.
    pipeline_.reset();
    ray_pipeline_ = std::static_pointer_cast<RayTracingPipelineObject>(record->shared_from_this());
    sets_.fill(nullptr);
    bound_set_mask_ = 0;
    use(*record);
    return core::success();
}

RhiStatus VulkanCommandList::dispatch_rays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
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
    // The limit matches the DX12 dispatch, which the compute path shares.
    constexpr std::uint32_t kMaxDispatchDimension = 65535;
    if (width > kMaxDispatchDimension || height > kMaxDispatchDimension || depth > kMaxDispatchDimension) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_rays: a dimension exceeds 65535");
    }
    if (const RhiStatus bound = check_bound_sets("dispatch_rays", ray_pipeline_->layouts); !bound.ok()) {
        return bound;
    }
    // The shader table holds the three records, and the callable stage is unused, so its region is empty.
    const VkStridedDeviceAddressRegionKHR no_callable{};
    device_.api().vkCmdTraceRaysKHR(data_->buffer, &ray_pipeline_->raygen, &ray_pipeline_->miss, &ray_pipeline_->hit, &no_callable, width, height,
                                    depth);
    return core::success();
}

RhiStatus VulkanCommandList::dispatch_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (const RhiStatus open = require_recording("dispatch_mesh"); !open.ok()) {
        return open;
    }
    if (!pass_active_ || !pipeline_ || pipeline_->kind != PipelineKind::Mesh) {
        return fail(RhiErrorCode::InvalidState, "dispatch_mesh: needs an active render pass and a bound mesh pipeline");
    }
    constexpr std::uint32_t kMaxMeshGroups = 65535;
    if (x == 0 || y == 0 || z == 0) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_mesh: group counts must be greater than zero");
    }
    if (x > kMaxMeshGroups || y > kMaxMeshGroups || z > kMaxMeshGroups) {
        return fail(RhiErrorCode::OutOfRange, "dispatch_mesh: a group count exceeds 65535");
    }
    if (const RhiStatus ready = check_compute_state("dispatch_mesh"); !ready.ok()) {
        return ready;
    }
    device_.api().vkCmdDrawMeshTasksEXT(data_->buffer, x, y, z);
    return core::success();
}

RhiStatus VulkanCommandList::copy_buffer(BufferHandle source, std::uint64_t source_offset, BufferHandle destination,
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
    VkBufferCopy region{};
    region.srcOffset = source_offset;
    region.dstOffset = destination_offset;
    region.size = size;
    device_.api().vkCmdCopyBuffer(data_->buffer, src->buffer, dst->buffer, 1, &region);
    use(*src);
    use(*dst);
    return core::success();
}

RhiStatus VulkanCommandList::copy_buffer_to_texture(BufferHandle source, std::uint64_t source_offset, TextureHandle destination) {
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

    VkBufferImageCopy region{};
    region.bufferOffset = source_offset;
    region.bufferRowLength = static_cast<std::uint32_t>(copy_row_pitch(dst->desc.width, dst->desc.format) / bytes_per_pixel(dst->desc.format));
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{dst->desc.width, dst->desc.height, 1};
    device_.api().vkCmdCopyBufferToImage(data_->buffer, src->buffer, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    use(*src);
    use(*dst);
    return core::success();
}

RhiStatus VulkanCommandList::copy_texture_to_buffer(TextureHandle source, BufferHandle destination, std::uint64_t destination_offset) {
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

    // Rows are written at the contract's pitch, which is a whole number of texels: 256 bytes divides by every supported texel size.
    VkBufferImageCopy region{};
    region.bufferOffset = destination_offset;
    region.bufferRowLength = static_cast<std::uint32_t>(copy_row_pitch(src->desc.width, src->desc.format) / bytes_per_pixel(src->desc.format));
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{src->desc.width, src->desc.height, 1};
    device_.api().vkCmdCopyImageToBuffer(data_->buffer, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->buffer, 1, &region);
    use(*src);
    use(*dst);
    return core::success();
}

RhiStatus VulkanCommandList::write_timestamp(QueryPoolHandle pool, std::uint32_t index) {
    if (const RhiStatus open = require_recording("write_timestamp"); !open.ok()) {
        return open;
    }
    if (!device_.capabilities().enabled(Feature::TimestampQueries)) {
        return fail(RhiErrorCode::FeatureDisabled, "write_timestamp: the TimestampQueries feature is not enabled");
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "write_timestamp: timestamps cannot be written inside a render pass");
    }
    QueryPoolObject* query = device_.find_query_pool(pool);
    if (query == nullptr) {
        return fail(RhiErrorCode::InvalidHandle, "write_timestamp: query pool handle is invalid or destroyed");
    }
    if (index >= query->count) {
        return fail(RhiErrorCode::OutOfRange, "write_timestamp: index is outside the query pool");
    }
    // A query is reset before each write, so a list may reuse an index that an earlier submission wrote.
    const Api& api = device_.api();
    api.vkCmdResetQueryPool(data_->buffer, query->pool, index, 1);
    api.vkCmdWriteTimestamp(data_->buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, query->pool, index);
    use(*query);
    timestamps_.emplace_back(std::static_pointer_cast<QueryPoolObject>(query->shared_from_this()), index);
    return core::success();
}

RhiStatus VulkanCommandList::close() {
    if (const RhiStatus open = require_recording("close"); !open.ok()) {
        return open;
    }
    if (pass_active_) {
        return fail(RhiErrorCode::InvalidState, "close: a render pass is still active");
    }
    if (const VkResult result = device_.api().vkEndCommandBuffer(data_->buffer); result != VK_SUCCESS) {
        return core::failure(vk_error("close: vkEndCommandBuffer", result));
    }
    // The recorded commands are final, so the list lets go of its bound pipeline and sets. Each one is already in refs_, which the
    // device holds until the submission completes; keeping a copy here would delay a destroyed set's storage past its fence.
    pipeline_.reset();
    ray_pipeline_.reset();
    sets_.fill(nullptr);
    state_ = ListState::Closed;
    return core::success();
}

}  // namespace engine::rhi::vulkan
