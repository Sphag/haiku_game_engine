#include "fsr2_common.hpp"

#include <atomic>
#include <cstdint>

namespace engine::rhi::fsr2 {
namespace {

std::atomic<std::uint32_t> g_error_count{0};

}  // namespace

FfxFsr2ContextDescription context_description(const UpscalerDesc& desc, bool debug_checks, const FfxFsr2Interface& callbacks,
                                              FfxDevice device) {
    FfxFsr2ContextDescription context{};
    context.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
    if (debug_checks) {
        context.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
    }
    context.maxRenderSize = FfxDimensions2D{desc.max_render_width, desc.max_render_height};
    context.displaySize = FfxDimensions2D{desc.display_width, desc.display_height};
    context.callbacks = callbacks;
    context.device = device;
    context.fpMessage = &on_message;
    return context;
}

FfxFsr2DispatchDescription dispatch_description(const UpscaleDispatch& dispatch, const Resources& resources, FfxCommandList list) {
    FfxFsr2DispatchDescription params{};
    params.commandList = list;
    params.color = resources.color;
    params.depth = resources.depth;
    params.motionVectors = resources.motion;
    params.exposure = resources.exposure;
    params.reactive = resources.reactive;
    params.transparencyAndComposition = FfxResource{};
    params.output = resources.output;
    params.jitterOffset = FfxFloatCoords2D{dispatch.jitter_x, dispatch.jitter_y};
    const auto width = static_cast<float>(dispatch.render_width);
    const auto height = static_cast<float>(dispatch.render_height);
    params.motionVectorScale = FfxFloatCoords2D{-width, -height};
    params.renderSize = FfxDimensions2D{dispatch.render_width, dispatch.render_height};
    params.enableSharpening = false;
    params.sharpness = 0.0f;
    params.frameTimeDelta = dispatch.frame_time_ms;
    params.preExposure = dispatch.pre_exposure;
    params.reset = dispatch.reset;
    params.cameraNear = dispatch.camera_near;
    params.cameraFar = dispatch.camera_far;
    params.cameraFovAngleVertical = dispatch.vertical_fov_radians;
    params.viewSpaceToMetersFactor = 1.0f;
    params.enableAutoReactive = false;
    return params;
}

std::uint32_t error_count() noexcept {
    return g_error_count.load(std::memory_order_relaxed);
}

void on_message(FfxFsr2MsgType type, const wchar_t* message) {
    (void)message;
    if (type == FFX_FSR2_MESSAGE_TYPE_ERROR) {
        g_error_count.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace engine::rhi::fsr2
