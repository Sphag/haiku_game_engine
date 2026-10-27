#include "vulkan/vulkan_common.hpp"

namespace engine::rhi::vulkan {

VkFormat to_vk(Format format) noexcept {
    switch (format) {
        case Format::Unknown:
            return VK_FORMAT_UNDEFINED;
        case Format::R8G8B8A8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8Srgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::B8G8R8A8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::R16G16B16A16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32Float:
            return VK_FORMAT_R32_SFLOAT;
        case Format::R32Uint:
            return VK_FORMAT_R32_UINT;
        case Format::D32Float:
            return VK_FORMAT_D32_SFLOAT;
        case Format::R32G32B32Float:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::R32G32Float:
            return VK_FORMAT_R32G32_SFLOAT;
        case Format::R32G32B32A32Float:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkSubpassDependency attachment_dependency(bool has_depth) noexcept {
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (has_depth) {
        const VkPipelineStageFlags depth_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcStageMask |= depth_stages;
        dependency.dstStageMask |= depth_stages;
        dependency.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    return dependency;
}

VkImageAspectFlags aspect_for(Format format) noexcept {
    return is_depth_format(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageUsageFlags image_usage(TextureUsage usage) noexcept {
    VkImageUsageFlags flags = 0;
    if (has_flags(usage, TextureUsage::ShaderResource)) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (has_flags(usage, TextureUsage::UnorderedAccess)) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (has_flags(usage, TextureUsage::RenderTarget)) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (has_flags(usage, TextureUsage::DepthStencil)) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (has_flags(usage, TextureUsage::CopySource)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (has_flags(usage, TextureUsage::CopyDestination)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

VkBufferUsageFlags buffer_usage(BufferUsage usage) noexcept {
    VkBufferUsageFlags flags = 0;
    if (has_flags(usage, BufferUsage::Vertex)) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (has_flags(usage, BufferUsage::Index)) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (has_flags(usage, BufferUsage::Constant)) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (has_flags(usage, BufferUsage::ShaderResource) || has_flags(usage, BufferUsage::UnorderedAccess)) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (has_flags(usage, BufferUsage::IndirectArgument)) {
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (has_flags(usage, BufferUsage::CopySource)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    // Readback memory is written by a copy, so it is a transfer destination whether or not the caller named CopyDestination.
    if (has_flags(usage, BufferUsage::CopyDestination) || has_flags(usage, BufferUsage::CpuRead)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

VkImageLayout layout_for(ResourceState state) noexcept {
    switch (state) {
        case ResourceState::Common:
        case ResourceState::UnorderedAccess:
            return VK_IMAGE_LAYOUT_GENERAL;
        case ResourceState::ShaderRead:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ResourceState::RenderTarget:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ResourceState::DepthWrite:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ResourceState::DepthRead:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ResourceState::CopySource:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ResourceState::CopyDestination:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ResourceState::Present:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case ResourceState::VertexBuffer:
        case ResourceState::IndexBuffer:
        case ResourceState::ConstantBuffer:
        case ResourceState::IndirectArgument:
            break;
    }
    // Buffer-only states never reach an image barrier; GENERAL keeps the answer valid if one does.
    return VK_IMAGE_LAYOUT_GENERAL;
}

SyncScope sync_for(ResourceState state) noexcept {
    // ALL_GRAPHICS also covers the task and mesh stages (US-038), which the core stage bits do not name. The extension's own stage bits
    // are not used, because they are invalid when the extension is not enabled.
    constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    switch (state) {
        case ResourceState::Common:
            // Common carries no known use, so a barrier from or to it orders against every access.
            return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        case ResourceState::VertexBuffer:
            return {VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
        case ResourceState::IndexBuffer:
            return {VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT};
        case ResourceState::ConstantBuffer:
            return {kShaderStages, VK_ACCESS_2_UNIFORM_READ_BIT};
        case ResourceState::ShaderRead:
            return {kShaderStages, VK_ACCESS_2_SHADER_READ_BIT};
        case ResourceState::UnorderedAccess:
            return {kShaderStages, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
        case ResourceState::RenderTarget:
            return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case ResourceState::DepthWrite:
            return {kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case ResourceState::DepthRead:
            return {kDepthStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
        case ResourceState::IndirectArgument:
            return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
        case ResourceState::CopySource:
            return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
        case ResourceState::CopyDestination:
            return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
        case ResourceState::Present:
            // The presentation engine is ordered by the semaphores the swapchain waits on, not by a memory access.
            return {VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0};
    }
    return {};
}

VkDescriptorType descriptor_type(DescriptorType type) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::ShaderResource:
        case DescriptorType::UnorderedAccess:
            return VK_DESCRIPTOR_TYPE_MUTABLE_EXT;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

MutableTypeList mutable_types(DescriptorType type, bool acceleration_structures) noexcept {
    MutableTypeList list;
    switch (type) {
        case DescriptorType::ConstantBuffer:
            return list;
        case DescriptorType::ShaderResource:
            list.types = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
            list.count = acceleration_structures ? 3 : 2;
            return list;
        case DescriptorType::UnorderedAccess:
            list.types = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
            list.count = 2;
            return list;
    }
    return list;
}

std::optional<VkDescriptorType> written_descriptor_type(DescriptorType type, bool texture) noexcept {
    switch (type) {
        case DescriptorType::ConstantBuffer:
            if (texture) {
                return std::nullopt;
            }
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::ShaderResource:
            return texture ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::UnorderedAccess:
            return texture ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    return std::nullopt;
}

VkImageLayout descriptor_image_layout(DescriptorType type) noexcept {
    return type == DescriptorType::ShaderResource ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
}

VkShaderStageFlags stage_flags(ShaderStage stages) noexcept {
    VkShaderStageFlags flags = 0;
    if (has_flags(stages, ShaderStage::Vertex)) {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (has_flags(stages, ShaderStage::Pixel)) {
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (has_flags(stages, ShaderStage::Compute)) {
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (has_flags(stages, ShaderStage::Mesh)) {
        flags |= VK_SHADER_STAGE_MESH_BIT_EXT;
    }
    if (has_flags(stages, ShaderStage::Amplification)) {
        flags |= VK_SHADER_STAGE_TASK_BIT_EXT;
    }
    if (has_flags(stages, ShaderStage::RayTracing)) {
        flags |= VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    }
    return flags;
}

VkBuildAccelerationStructureFlagsKHR acceleration_build_flags(bool allow_refit) noexcept {
    VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (allow_refit) {
        flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    }
    return flags;
}

VkGeometryFlagsKHR geometry_flags(bool opaque) noexcept {
    return opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
}

std::optional<std::uint32_t> find_memory_type(const VkPhysicalDeviceMemoryProperties& properties, std::uint32_t type_bits,
                                              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) noexcept {
    std::optional<std::uint32_t> fallback;
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((type_bits & (1u << index)) == 0) {
            continue;
        }
        const VkMemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return index;
        }
        if (!fallback) {
            fallback = index;
        }
    }
    return fallback;
}

bool presents_with_tearing(bool vsync_requested, bool allow_tearing_requested, bool immediate_supported) noexcept {
    return !vsync_requested && allow_tearing_requested && immediate_supported;
}

}  // namespace engine::rhi::vulkan
