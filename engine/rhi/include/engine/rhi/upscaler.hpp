#pragma once

// Temporal upscaling (US-046). The RHI runs FidelityFX FSR 2 on both backends behind this contract: the caller names the textures
// and the jitter, and the backend turns them into FSR 2's own resources. No DirectX or Vulkan type appears here.

#include "engine/rhi/error.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <cstdint>

namespace engine::rhi {

// Size and fixed settings of one upscaler. The internal (render) size may be any size up to the maximum; the display size is the output.
struct UpscalerDesc {
    std::uint32_t max_render_width = 0;
    std::uint32_t max_render_height = 0;
    std::uint32_t display_width = 0;
    std::uint32_t display_height = 0;
};

// The inputs and the output of one upscale. Every texture must be a current texture of the device, with the usage named below.
//
//   color     render size, ShaderResource, HDR linear color (R16G16B16A16Float)
//   depth     render size, ShaderResource, R32Float, standard depth (0 at the near plane, 1 at the far plane)
//   motion    render size, ShaderResource, R16G16B16A16Float (xy), UV units, x right and y down; previous = current - motion
//   reactive  optional, render size, ShaderResource, R32Float; 0 for ordinary surfaces, 1 for transparent or reactive ones
//   exposure  optional, 1x1, ShaderResource, R32Float; the exposure value FSR reads with auto exposure
//   output    display size, UnorderedAccess, R16G16B16A16Float
//
// The inputs and the output keep their tracked state across the call: the caller transitions them to the states above before the call,
// and they are in the same states after it. The command list leaves no pipeline or descriptor set bound after an upscale, so the caller
// binds them again before the next dispatch.
struct UpscaleDispatch {
    TextureHandle color;
    TextureHandle depth;
    TextureHandle motion;
    TextureHandle reactive;   // invalid handle when there is no reactive mask
    TextureHandle exposure;   // invalid handle when there is no exposure texture
    TextureHandle output;

    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    // The sub-pixel offset the projection applied this frame, in internal pixels, each component in [-0.5, 0.5]. FSR 2 needs the same
    // sequence that moved the camera (fsr_jitter in engine/render/upscaler.hpp).
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    // True when the history is discarded: the first frame, a size change, or a camera cut.
    bool reset = false;
    float frame_time_ms = 16.7f;
    float pre_exposure = 1.0f;     // must be greater than zero
    float camera_near = 0.1f;
    float camera_far = 1000.0f;
    float vertical_fov_radians = 1.0471976f;  // 60 degrees
};

// The inputs and the output of an upscale, for check_upscale_texture.
enum class UpscaleRole : std::uint8_t { Color, Depth, Motion, Reactive, Exposure, Output };

// Rejects a dispatch the upscaler cannot run: a render size of zero or above the upscaler's maximum, a jitter outside half a pixel, a
// non-positive pre-exposure, or camera planes or a field of view out of range. It does not check the textures (check_upscale_texture does).
[[nodiscard]] RhiStatus validate_upscale_dispatch(const UpscalerDesc& desc, const UpscaleDispatch& dispatch);

// Checks one texture of a dispatch against its role: the state the caller left it in, its usage flags, its format, and its size. Inputs
// must cover the render size, and the exposure texture needs one texel. The output must be the display size.
[[nodiscard]] RhiStatus check_upscale_texture(UpscaleRole role, const UpscalerDesc& desc, const UpscaleDispatch& dispatch,
                                              const TextureDesc& texture, ResourceState state);

// One FSR 2 context. It holds the upscaler's history, so it is created once per render size and display size and reused every frame.
// It is destroyed before the device it was created from.
class Upscaler {
public:
    virtual ~Upscaler() = default;
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;

    [[nodiscard]] virtual const UpscalerDesc& desc() const noexcept = 0;

protected:
    Upscaler() = default;
};

}  // namespace engine::rhi
