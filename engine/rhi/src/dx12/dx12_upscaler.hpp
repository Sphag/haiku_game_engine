#pragma once

// FSR 2 on DX12 (US-046). The upscaler owns one FSR 2 context and records each upscale into the caller's command list. The command list
// resolves the textures and checks their states before it calls record().

#include "dx12/dx12_device.hpp"

#include "engine/rhi/error.hpp"
#include "engine/rhi/upscaler.hpp"

#include "ffx_fsr2.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace engine::rhi::dx12 {

// The textures of one upscale. A null pointer is an absent optional input.
struct UpscaleImages {
    TextureObject* color = nullptr;
    TextureObject* depth = nullptr;
    TextureObject* motion = nullptr;
    TextureObject* reactive = nullptr;
    TextureObject* exposure = nullptr;
    TextureObject* output = nullptr;
};

class Dx12Upscaler final : public Upscaler {
public:
    Dx12Upscaler(Dx12Device& device, const UpscalerDesc& desc);
    ~Dx12Upscaler() override;

    // Creates the FSR 2 context. Fails with Unsupported when FSR 2 cannot run on this device.
    [[nodiscard]] RhiStatus initialize(bool debug_checks);

    [[nodiscard]] const UpscalerDesc& desc() const noexcept override {
        return desc_;
    }
    [[nodiscard]] std::uint32_t device_id() const noexcept {
        return device_id_;
    }

    // Records one upscale into `list`. The command list has checked the handles, the states, the usage flags, and the sizes.
    [[nodiscard]] RhiStatus record(ID3D12GraphicsCommandList* list, const UpscaleImages& images, const UpscaleDispatch& dispatch);

private:
    [[nodiscard]] FfxResource wrap(TextureObject& texture, const wchar_t* name, FfxResourceStates state);

    UpscalerDesc desc_;
    std::uint32_t device_id_ = 0;
    Dx12Device* device_ = nullptr;  // the device outlives the upscaler; its queues are drained before the context is destroyed
    ID3D12Device* native_device_ = nullptr;
    std::vector<std::uint64_t> scratch_;  // FSR 2's backend state; the interface points into it, so it lives as long as the context
    FfxFsr2Interface interface_{};
    std::unique_ptr<FfxFsr2Context> context_;  // heap-held: the context is large
};

}  // namespace engine::rhi::dx12
