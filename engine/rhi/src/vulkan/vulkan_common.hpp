#pragma once

// Pure mapping from RHI types to Vulkan enums, and the present-mode policy. Nothing here touches a device, so the tables compile and
// test on every OS, including the CPU-only unit runs in CI.

#include "vulkan/vulkan_api.hpp"

#include "engine/rhi/types.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace engine::rhi::vulkan {

[[nodiscard]] VkFormat to_vk(Format format) noexcept;
[[nodiscard]] VkImageAspectFlags aspect_for(Format format) noexcept;
// The dependency every render pass of this backend carries: earlier writes to its attachments finish before it touches them again. Begin
// and pipeline creation share it, so the pipeline's pass and the pass it is drawn in are the same description.
[[nodiscard]] VkSubpassDependency attachment_dependency(bool has_depth) noexcept;
[[nodiscard]] VkImageUsageFlags image_usage(TextureUsage usage) noexcept;
[[nodiscard]] VkBufferUsageFlags buffer_usage(BufferUsage usage) noexcept;

// The image layout a tracked texture state requires. Common maps to GENERAL, which every usage accepts.
[[nodiscard]] VkImageLayout layout_for(ResourceState state) noexcept;

// The pipeline stages and memory access that a tracked state implies, in synchronization2 flags (VK_KHR_synchronization2). A barrier
// takes one of these for its source (the state before) and one for its destination (the state after).
struct SyncScope {
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
};
[[nodiscard]] SyncScope sync_for(ResourceState state) noexcept;

// Descriptor mapping (US-030). A ShaderResource or UnorderedAccess binding takes either a buffer or a texture, and the RHI does not
// say which at layout time, so it is VK_DESCRIPTOR_TYPE_MUTABLE_EXT. The mutable list names the types it may hold. A constant buffer
// is always a uniform buffer.
[[nodiscard]] VkDescriptorType descriptor_type(DescriptorType type) noexcept;
// The types a mutable binding may hold. Empty for a fixed binding, whose list is ignored.
struct MutableTypeList {
    std::array<VkDescriptorType, 3> types{};
    std::uint32_t count = 0;
};
// A shader resource binding also holds a top-level acceleration structure when the device has ray tracing (US-042), so the
// acceleration structure type joins its list only then.
[[nodiscard]] MutableTypeList mutable_types(DescriptorType type, bool acceleration_structures = false) noexcept;
// The descriptor type a written resource takes: a buffer or a texture in a binding of `type`. Returns nothing when the binding cannot
// hold that kind of resource (a texture in a constant buffer binding).
[[nodiscard]] std::optional<VkDescriptorType> written_descriptor_type(DescriptorType type, bool texture) noexcept;
// The image layout a descriptor reads or writes an image with: shader-read for ShaderResource, general for UnorderedAccess.
[[nodiscard]] VkImageLayout descriptor_image_layout(DescriptorType type) noexcept;
// Shader stage flags for a binding's visibility.
[[nodiscard]] VkShaderStageFlags stage_flags(ShaderStage stages) noexcept;

// Build flags of an acceleration structure (US-042). A structure that allows refit is built with ALLOW_UPDATE, which an update needs.
[[nodiscard]] VkBuildAccelerationStructureFlagsKHR acceleration_build_flags(bool allow_refit) noexcept;
// Geometry flags of one bottom-level geometry: opaque geometry skips any-hit evaluation.
[[nodiscard]] VkGeometryFlagsKHR geometry_flags(bool opaque) noexcept;

// Index of a memory type that has every bit in `required` and, when possible, every bit in `preferred`. Returns nothing when no type
// matches `required` among the types in `type_bits`.
[[nodiscard]] std::optional<std::uint32_t> find_memory_type(const VkPhysicalDeviceMemoryProperties& properties, std::uint32_t type_bits,
                                                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) noexcept;

// Whether a swapchain presents without waiting for vertical blank. Tearing needs the request and immediate-mode support; otherwise
// vsync stays on, and the caller reports the effective policy.
[[nodiscard]] bool presents_with_tearing(bool vsync_requested, bool allow_tearing_requested, bool immediate_supported) noexcept;

}  // namespace engine::rhi::vulkan
