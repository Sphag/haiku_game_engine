#pragma once
// FidelityFX FSR 2 glue shared by the DX12 and Vulkan upscalers (US-046). It holds what the backends have in common: the context and dispatch
// descriptions in FSR 2's terms, the input checks, and the message counter that FSR's callback feeds. The backends supply the native device,
// command list, and resources. The vendored FSR 2 headers are included as system headers, so their warnings never reach -Werror.

#include "engine/rhi/error.hpp"
#include "engine/rhi/upscaler.hpp"

#include <cstddef>
#include <cstdint>

// FSR 2 is built only when its shader permutation headers are vendored (CMakeLists.txt sets this). Without them the backends report
// Unsupported and the FSR 2 headers are not needed by the backends.
#ifndef ENGINE_FSR2_AVAILABLE
#define ENGINE_FSR2_AVAILABLE 0
#endif

#include "ffx_fsr2.h"

namespace engine::rhi::fsr2 {

// Resource handles of one dispatch, already in FSR 2's form. A null resource stands for an optional input that is absent.
struct Resources {
    FfxResource color{};
    FfxResource depth{};
    FfxResource motion{};
    FfxResource reactive{};
    FfxResource exposure{};
    FfxResource output{};
};

// The context FSR 2 is created with. The color is HDR linear, depth is standard (not inverted, not infinite), the motion vectors are
// unjittered, and auto exposure is off (the caller passes preExposure). Debug checking follows the device's debug layers.
[[nodiscard]] FfxFsr2ContextDescription context_description(const UpscalerDesc& desc, bool debug_checks, const FfxFsr2Interface& callbacks,
                                                            FfxDevice device);

// The dispatch of one upscale. The motion vectors are in UV units with previous = current - motion (the engine's convention, scene.hlsl).
// FSR 2 reads previous = current + motion * scale / motion size, so the scale is the negated render size.
[[nodiscard]] FfxFsr2DispatchDescription dispatch_description(const UpscaleDispatch& dispatch, const Resources& resources,
                                                              FfxCommandList list);

// FSR 2's callback takes no context, so every upscaler shares one count. A dispatch reports an error when the count rose during it.
[[nodiscard]] std::uint32_t error_count() noexcept;
void on_message(FfxFsr2MsgType type, const wchar_t* message);

}  // namespace engine::rhi::fsr2
