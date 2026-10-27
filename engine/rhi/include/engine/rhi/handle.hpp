#pragma once

#include <cstdint>

namespace engine::rhi {

// Typed reference to an object owned by one Device. The index selects a slot, and the generation changes every time that
// slot is freed, so a handle kept after its object was destroyed is rejected even when the slot is reused. The device id ties
// the handle to the device that created it. A handle holds no backend pointer, so scene and ECS code can store it without
// depending on DX12 or Vulkan.
template <typename Tag>
struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;  // 0 is never a live generation
    std::uint32_t device = 0;      // 0 is never a live device id

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation != 0 && device != 0;
    }

    friend constexpr bool operator==(const Handle& lhs, const Handle& rhs) noexcept {
        return lhs.index == rhs.index && lhs.generation == rhs.generation && lhs.device == rhs.device;
    }
    friend constexpr bool operator!=(const Handle& lhs, const Handle& rhs) noexcept {
        return !(lhs == rhs);
    }
};

struct BufferTag {};
struct TextureTag {};
struct DescriptorSetLayoutTag {};
struct DescriptorSetTag {};
struct PipelineTag {};
struct FenceTag {};
struct QueryPoolTag {};
struct AccelerationStructureTag {};
struct RayTracingPipelineTag {};

using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using DescriptorSetLayoutHandle = Handle<DescriptorSetLayoutTag>;
using DescriptorSetHandle = Handle<DescriptorSetTag>;
using PipelineHandle = Handle<PipelineTag>;
using FenceHandle = Handle<FenceTag>;
using QueryPoolHandle = Handle<QueryPoolTag>;
using AccelerationStructureHandle = Handle<AccelerationStructureTag>;
using RayTracingPipelineHandle = Handle<RayTracingPipelineTag>;

}  // namespace engine::rhi
