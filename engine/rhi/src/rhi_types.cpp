#include "engine/rhi/pipeline_rules.hpp"
#include "engine/rhi/capabilities.hpp"
#include "engine/rhi/error.hpp"
#include "engine/rhi/types.hpp"

#include "engine/core/result.hpp"

namespace engine::rhi {

bool buffer_state_allowed(BufferUsage usage, ResourceState state) noexcept {
    // Readback memory is only ever in CopyDestination, so it allows no other state.
    if (has_flags(usage, BufferUsage::CpuRead) && state != ResourceState::CopyDestination) {
        return false;
    }
    // Upload memory stays in its one state, so it allows CopySource and nothing else.
    if (has_flags(usage, BufferUsage::CpuWrite) && state != ResourceState::CopySource) {
        return false;
    }
    switch (state) {
        case ResourceState::Common:
            return true;
        case ResourceState::VertexBuffer:
            return has_flags(usage, BufferUsage::Vertex);
        case ResourceState::IndexBuffer:
            return has_flags(usage, BufferUsage::Index);
        case ResourceState::ConstantBuffer:
            return has_flags(usage, BufferUsage::Constant);
        case ResourceState::ShaderRead:
            return has_flags(usage, BufferUsage::ShaderResource);
        case ResourceState::UnorderedAccess:
            return has_flags(usage, BufferUsage::UnorderedAccess);
        case ResourceState::IndirectArgument:
            return has_flags(usage, BufferUsage::IndirectArgument);
        case ResourceState::CopySource:
            return has_flags(usage, BufferUsage::CopySource);
        case ResourceState::CopyDestination:
            return has_flags(usage, BufferUsage::CopyDestination);
        case ResourceState::RenderTarget:
        case ResourceState::DepthWrite:
        case ResourceState::DepthRead:
        case ResourceState::Present:
            return false;
    }
    return false;
}

bool texture_state_allowed(TextureUsage usage, ResourceState state) noexcept {
    switch (state) {
        case ResourceState::Common:
            return true;
        case ResourceState::ShaderRead:
            return has_flags(usage, TextureUsage::ShaderResource);
        case ResourceState::UnorderedAccess:
            return has_flags(usage, TextureUsage::UnorderedAccess);
        case ResourceState::RenderTarget:
        case ResourceState::Present:
            return has_flags(usage, TextureUsage::RenderTarget);
        case ResourceState::DepthWrite:
        case ResourceState::DepthRead:
            return has_flags(usage, TextureUsage::DepthStencil);
        case ResourceState::CopySource:
            return has_flags(usage, TextureUsage::CopySource);
        case ResourceState::CopyDestination:
            return has_flags(usage, TextureUsage::CopyDestination);
        case ResourceState::VertexBuffer:
        case ResourceState::IndexBuffer:
        case ResourceState::ConstantBuffer:
        case ResourceState::IndirectArgument:
            return false;
    }
    return false;
}

std::string_view to_string(Backend backend) noexcept {
    switch (backend) {
        case Backend::Validation:
            return "validation";
        case Backend::Dx12:
            return "dx12";
        case Backend::Vulkan:
            return "vulkan";
    }
    return "unknown";
}

std::string_view to_string(QueueType queue) noexcept {
    switch (queue) {
        case QueueType::Graphics:
            return "graphics";
        case QueueType::Compute:
            return "compute";
        case QueueType::Copy:
            return "copy";
    }
    return "unknown";
}

std::string_view to_string(FeatureState state) noexcept {
    switch (state) {
        case FeatureState::Unsupported:
            return "unsupported";
        case FeatureState::DisabledForTest:
            return "disabled-for-test";
        case FeatureState::Supported:
            return "supported";
    }
    return "unknown";
}

}  // namespace engine::rhi

namespace engine::rhi {

RhiStatus check_vertex_layout(const PipelineDesc& desc) {
    if (desc.vertex_attributes.empty()) {
        if (desc.vertex_stride != 0) {
            return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: a vertex stride needs vertex attributes");
        }
        if (!desc.instance_attributes.empty() || desc.instance_stride != 0) {
            return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: per-instance input needs vertex attributes");
        }
        return core::success();
    }
    if (desc.kind != PipelineKind::Graphics) {
        return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: only graphics pipelines take vertex input");
    }
    if (desc.vertex_stride == 0) {
        return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: vertex input needs a non-zero stride");
    }
    if (desc.instance_attributes.empty() != (desc.instance_stride == 0)) {
        return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: per-instance input needs both attributes and a non-zero stride");
    }
    // Every attribute of both slots: a supported format inside its stride, and one location per attribute across the two slots.
    std::vector<VertexAttribute> all = desc.vertex_attributes;
    all.insert(all.end(), desc.instance_attributes.begin(), desc.instance_attributes.end());
    for (std::size_t i = 0; i < all.size(); ++i) {
        const VertexAttribute& attribute = all[i];
        const bool instance = i >= desc.vertex_attributes.size();
        const std::uint32_t stride = instance ? desc.instance_stride : desc.vertex_stride;
        if (attribute.format != Format::R32G32B32Float && attribute.format != Format::R32G32Float && attribute.format != Format::R32G32B32A32Float &&
            attribute.format != Format::R32Uint) {
            return fail(RhiErrorCode::InvalidDescriptor,
                        "create_pipeline: the vertex format is not supported (use R32G32B32Float, R32G32Float, R32G32B32A32Float, or R32Uint)");
        }
        if (static_cast<std::uint64_t>(attribute.offset) + bytes_per_pixel(attribute.format) > stride) {
            return fail(RhiErrorCode::OutOfRange, "create_pipeline: a vertex attribute lies outside the vertex stride");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (all[j].location == attribute.location) {
                return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: two vertex attributes share a location");
            }
        }
    }
    return core::success();
}

RhiStatus check_blend_state(const PipelineDesc& desc) {
    if (!desc.alpha_blend && !desc.additive_blend) {
        return core::success();
    }
    if (desc.alpha_blend && desc.additive_blend) {
        return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: alpha and additive blending are exclusive");
    }
    if (desc.kind != PipelineKind::Graphics) {
        return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: only graphics pipelines blend");
    }
    if (desc.color_formats.empty()) {
        return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: blending needs a color target");
    }
    for (const Format format : desc.color_formats) {
        if (!is_blendable(format)) {
            return fail(RhiErrorCode::InvalidDescriptor, "create_pipeline: blending needs blendable color formats");
        }
    }
    return core::success();
}

}  // namespace engine::rhi
