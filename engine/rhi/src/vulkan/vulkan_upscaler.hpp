#pragma once

// FSR 2 on Vulkan (US-046). The upscaler owns one FSR 2 context and records each upscale into the caller's command buffer. The command
// list resolves the textures and checks their states before it calls record().

#include "vulkan/vulkan_device.hpp"

#include "engine/rhi/error.hpp"
#include "engine/rhi/upscaler.hpp"

#include "ffx_fsr2.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace engine::rhi::vulkan {

// The textures of one upscale. A null pointer is an absent optional input.
struct UpscaleImages {
    TextureObject* color = nullptr;
    TextureObject* depth = nullptr;
    TextureObject* motion = nullptr;
    TextureObject* reactive = nullptr;
    TextureObject* exposure = nullptr;
    TextureObject* output = nullptr;
};

class VulkanUpscaler final : public Upscaler {
public:
    VulkanUpscaler(VulkanDevice& device, const UpscalerDesc& desc);
    ~VulkanUpscaler() override;

    // Creates the FSR 2 context. Fails with Unsupported when FSR 2 cannot run on this device.
    [[nodiscard]] RhiStatus initialize(bool debug_checks);

    [[nodiscard]] const UpscalerDesc& desc() const noexcept override {
        return desc_;
    }
    [[nodiscard]] std::uint32_t device_id() const noexcept {
        return device_id_;
    }

    // Records one upscale into `command`. The command list has checked the handles, the states, the usage flags, and the sizes.
    [[nodiscard]] RhiStatus record(VkCommandBuffer command, const UpscaleImages& images, const UpscaleDispatch& dispatch);

private:
    [[nodiscard]] FfxResource wrap(TextureObject& texture, const wchar_t* name, FfxResourceStates state);

    UpscalerDesc desc_;
    std::uint32_t device_id_ = 0;
    DeviceRef owner_;  // the device's entry points and handle; the device outlives the upscaler
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    std::vector<std::uint64_t> scratch_;  // FSR 2's backend state; the interface points into it, so it lives as long as the context
    FfxFsr2Interface interface_{};
    std::unique_ptr<FfxFsr2Context> context_;  // heap-held: the context is large
};

}  // namespace engine::rhi::vulkan
