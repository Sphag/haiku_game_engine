#include "dx12/dx12_upscaler.hpp"

#include "fsr2_common.hpp"

#include "ffx_fsr2_dx12.h"

namespace engine::rhi::dx12 {

Dx12Upscaler::Dx12Upscaler(Dx12Device& device, const UpscalerDesc& desc)
    : desc_(desc), device_id_(device.id()), device_(&device), native_device_(device.native()) {}

Dx12Upscaler::~Dx12Upscaler() {
    if (context_) {
        // FSR 2 may still have work in flight in the command lists it recorded, so the device drains before its context goes.
        device_->wait_idle();
        ffxFsr2ContextDestroy(context_.get());
    }
}

RhiStatus Dx12Upscaler::initialize(bool debug_checks) {
    const std::size_t scratch_bytes = ffxFsr2GetScratchMemorySizeDX12();
    scratch_.assign((scratch_bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
    const FfxErrorCode interface_status =
        ffxFsr2GetInterfaceDX12(&interface_, native_device_, scratch_.data(), scratch_.size() * sizeof(std::uint64_t));
    if (interface_status != FFX_OK) {
        return fail(RhiErrorCode::Unsupported, "create_upscaler: FSR 2 cannot use this DX12 device (interface error)");
    }
    const FfxFsr2ContextDescription description = fsr2::context_description(desc_, debug_checks, interface_, ffxGetDeviceDX12(native_device_));
    auto context = std::make_unique<FfxFsr2Context>();
    const FfxErrorCode status = ffxFsr2ContextCreate(context.get(), &description);
    if (status != FFX_OK) {
        return fail(RhiErrorCode::Unsupported, "create_upscaler: FSR 2 context creation failed on this DX12 device");
    }
    context_ = std::move(context);
    return core::success();
}

FfxResource Dx12Upscaler::wrap(TextureObject& texture, const wchar_t* name, FfxResourceStates state) {
    return ffxGetResourceDX12(context_.get(), texture.resource.Get(), name, state);
}

RhiStatus Dx12Upscaler::record(ID3D12GraphicsCommandList* list, const UpscaleImages& images, const UpscaleDispatch& dispatch) {
    const std::uint32_t errors_before = fsr2::error_count();
    fsr2::Resources resources;
    resources.color = wrap(*images.color, L"fsr2 color", FFX_RESOURCE_STATE_COMPUTE_READ);
    resources.depth = wrap(*images.depth, L"fsr2 depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    resources.motion = wrap(*images.motion, L"fsr2 motion", FFX_RESOURCE_STATE_COMPUTE_READ);
    if (images.reactive != nullptr) {
        resources.reactive = wrap(*images.reactive, L"fsr2 reactive", FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    if (images.exposure != nullptr) {
        resources.exposure = wrap(*images.exposure, L"fsr2 exposure", FFX_RESOURCE_STATE_COMPUTE_READ);
    }
    resources.output = wrap(*images.output, L"fsr2 output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxFsr2DispatchDescription params = fsr2::dispatch_description(dispatch, resources, ffxGetCommandListDX12(list));
    const FfxErrorCode status = ffxFsr2ContextDispatch(context_.get(), &params);
    if (status != FFX_OK || fsr2::error_count() != errors_before) {
        return fail(RhiErrorCode::InvalidState, "upscale: FSR 2 reported an error for this dispatch (see the FSR 2 messages)");
    }
    return core::success();
}

}  // namespace engine::rhi::dx12
