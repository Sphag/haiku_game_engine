// CPU tests for the Vulkan backend's pure mapping: RHI formats, usages, states, memory types, and the present-mode policy. These run on
// every OS without a driver, so CI's CPU jobs cover the tables that the GPU suites depend on.

#include "vulkan/vulkan_common.hpp"

#include "engine/rhi/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using engine::rhi::BufferUsage;
using engine::rhi::copy_row_pitch;
using engine::rhi::Format;
using engine::rhi::ResourceState;
using engine::rhi::TextureUsage;
namespace vk = engine::rhi::vulkan;

TEST_CASE("Vulkan formats map to the matching VkFormat", "[unit][rhi][vulkan]") {
    CHECK(vk::to_vk(Format::B8G8R8A8Unorm) == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(vk::to_vk(Format::R8G8B8A8Unorm) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(vk::to_vk(Format::R8G8B8A8Srgb) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(vk::to_vk(Format::R16G16B16A16Float) == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(vk::to_vk(Format::R32Float) == VK_FORMAT_R32_SFLOAT);
    CHECK(vk::to_vk(Format::R32Uint) == VK_FORMAT_R32_UINT);
    CHECK(vk::to_vk(Format::D32Float) == VK_FORMAT_D32_SFLOAT);
    CHECK(vk::to_vk(Format::Unknown) == VK_FORMAT_UNDEFINED);
}

TEST_CASE("Depth formats use the depth aspect and color formats the color aspect", "[unit][rhi][vulkan]") {
    CHECK(vk::aspect_for(Format::D32Float) == VK_IMAGE_ASPECT_DEPTH_BIT);
    CHECK(vk::aspect_for(Format::B8G8R8A8Unorm) == VK_IMAGE_ASPECT_COLOR_BIT);
}

TEST_CASE("Copy row pitch is a whole number of texels for every copyable format", "[unit][rhi][vulkan]") {
    // The Vulkan copy takes its row length in texels, so the 256-byte pitch must divide evenly by each texel size.
    for (Format format : {Format::R8G8B8A8Unorm, Format::B8G8R8A8Unorm, Format::R16G16B16A16Float, Format::R32Float, Format::R32Uint}) {
        const std::uint32_t texel = engine::rhi::bytes_per_pixel(format);
        for (std::uint32_t width : {1u, 63u, 64u, 65u, 1000u}) {
            CHECK(copy_row_pitch(width, format) % texel == 0);
        }
    }
}

TEST_CASE("Texture usage maps to image usage, one flag per use", "[unit][rhi][vulkan]") {
    CHECK(vk::image_usage(TextureUsage::RenderTarget) == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    CHECK(vk::image_usage(TextureUsage::DepthStencil) == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    CHECK(vk::image_usage(TextureUsage::ShaderResource) == VK_IMAGE_USAGE_SAMPLED_BIT);
    CHECK(vk::image_usage(TextureUsage::UnorderedAccess) == VK_IMAGE_USAGE_STORAGE_BIT);
    CHECK(vk::image_usage(TextureUsage::CopySource) == VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    CHECK(vk::image_usage(TextureUsage::CopyDestination) == VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    CHECK(vk::image_usage(TextureUsage::RenderTarget | TextureUsage::CopySource) ==
          (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
}

TEST_CASE("Buffer usage maps to buffer usage, and readback is always a transfer destination", "[unit][rhi][vulkan]") {
    CHECK(vk::buffer_usage(BufferUsage::Vertex) == VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    CHECK(vk::buffer_usage(BufferUsage::Index) == VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    CHECK(vk::buffer_usage(BufferUsage::Constant) == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    CHECK(vk::buffer_usage(BufferUsage::IndirectArgument) == VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    CHECK(vk::buffer_usage(BufferUsage::UnorderedAccess) == VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    CHECK(vk::buffer_usage(BufferUsage::CpuRead | BufferUsage::CopyDestination) == VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    CHECK(vk::buffer_usage(BufferUsage::CpuRead) == VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}

TEST_CASE("Tracked resource states map to the image layout they require", "[unit][rhi][vulkan]") {
    CHECK(vk::layout_for(ResourceState::Common) == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(vk::layout_for(ResourceState::RenderTarget) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(vk::layout_for(ResourceState::DepthWrite) == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    CHECK(vk::layout_for(ResourceState::DepthRead) == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    CHECK(vk::layout_for(ResourceState::ShaderRead) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vk::layout_for(ResourceState::CopySource) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(vk::layout_for(ResourceState::CopyDestination) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(vk::layout_for(ResourceState::Present) == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

TEST_CASE("A tracked state's synchronization scope names the stage and access it uses", "[unit][rhi][vulkan]") {
    const vk::SyncScope render = vk::sync_for(ResourceState::RenderTarget);
    CHECK(render.stages == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK((render.access & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0);

    const vk::SyncScope copy_source = vk::sync_for(ResourceState::CopySource);
    CHECK(copy_source.stages == VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK(copy_source.access == VK_ACCESS_TRANSFER_READ_BIT);

    const vk::SyncScope copy_destination = vk::sync_for(ResourceState::CopyDestination);
    CHECK(copy_destination.access == VK_ACCESS_TRANSFER_WRITE_BIT);

    // Presentation is ordered by the swapchain's semaphores, so the Present state carries no memory access.
    const vk::SyncScope present = vk::sync_for(ResourceState::Present);
    CHECK(present.stages == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    CHECK(present.access == 0);
}

TEST_CASE("The memory type search honors required flags and prefers the preferred ones", "[unit][rhi][vulkan]") {
    VkPhysicalDeviceMemoryProperties properties{};
    properties.memoryTypeCount = 3;
    properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    properties.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    properties.memoryTypes[2].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    const VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // Type 2 has every preferred flag, so it wins over type 1, which is also host-visible.
    CHECK(vk::find_memory_type(properties, 0b111, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, cached) == 2u);
    // Without type 2, the search falls back to the host-visible type that matches the required flags.
    CHECK(vk::find_memory_type(properties, 0b011, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, cached) == 1u);
    // A type the resource does not allow (bit clear in type_bits) is never chosen, even if it matches.
    CHECK(vk::find_memory_type(properties, 0b101, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0) == 2u);
    // Nothing matches the required flags.
    CHECK_FALSE(vk::find_memory_type(properties, 0b001, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0).has_value());
    CHECK(vk::find_memory_type(properties, 0b111, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0u);
}

TEST_CASE("Tearing is used only when vsync is off, tearing is requested, and immediate mode exists", "[unit][rhi][vulkan]") {
    CHECK(vk::presents_with_tearing(false, true, true));
    CHECK_FALSE(vk::presents_with_tearing(true, true, true));
    CHECK_FALSE(vk::presents_with_tearing(false, false, true));
    CHECK_FALSE(vk::presents_with_tearing(false, true, false));
}

TEST_CASE("Descriptor bindings map to Vulkan types, with shader resources and UAVs mutable", "[unit][rhi][vulkan]") {
    using engine::rhi::DescriptorType;
    using engine::rhi::ShaderStage;
    // A constant buffer is always a uniform buffer; the RHI does not say whether a resource or UAV binding holds a buffer or an image.
    CHECK(vk::descriptor_type(DescriptorType::ConstantBuffer) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK(vk::descriptor_type(DescriptorType::ShaderResource) == VK_DESCRIPTOR_TYPE_MUTABLE_EXT);
    CHECK(vk::descriptor_type(DescriptorType::UnorderedAccess) == VK_DESCRIPTOR_TYPE_MUTABLE_EXT);

    const vk::MutableTypeList srv = vk::mutable_types(DescriptorType::ShaderResource);
    REQUIRE(srv.count == 2);
    CHECK(srv.types[0] == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    CHECK(srv.types[1] == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    CHECK(vk::mutable_types(DescriptorType::ConstantBuffer).count == 0);
    // A shader resource binding also holds a top-level structure once the device has ray tracing (US-042).
    const vk::MutableTypeList with_structures = vk::mutable_types(DescriptorType::ShaderResource, true);
    REQUIRE(with_structures.count == 3);
    CHECK(with_structures.types[2] == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);

    // A texture can fill a shader resource or a UAV binding, and a constant buffer takes buffers only.
    CHECK(vk::written_descriptor_type(DescriptorType::ShaderResource, false) == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    CHECK(vk::written_descriptor_type(DescriptorType::ShaderResource, true) == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    CHECK(vk::written_descriptor_type(DescriptorType::UnorderedAccess, true) == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    CHECK(vk::written_descriptor_type(DescriptorType::ConstantBuffer, false) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK_FALSE(vk::written_descriptor_type(DescriptorType::ConstantBuffer, true).has_value());

    CHECK(vk::descriptor_image_layout(DescriptorType::ShaderResource) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vk::descriptor_image_layout(DescriptorType::UnorderedAccess) == VK_IMAGE_LAYOUT_GENERAL);

    CHECK(vk::stage_flags(ShaderStage::Compute) == VK_SHADER_STAGE_COMPUTE_BIT);
    CHECK(vk::stage_flags(ShaderStage::Compute | ShaderStage::Pixel) == (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT));
    // Ray tracing visibility covers the three stages a ray tracing pipeline runs (US-042).
    CHECK(vk::stage_flags(ShaderStage::RayTracing) ==
          (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR));
    CHECK(vk::stage_flags(ShaderStage::None) == 0);
}
