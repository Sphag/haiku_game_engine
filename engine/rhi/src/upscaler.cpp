#include "engine/rhi/upscaler.hpp"

#include <cmath>
#include <cstddef>
#include <string>

namespace engine::rhi {

RhiStatus validate_upscale_dispatch(const UpscalerDesc& desc, const UpscaleDispatch& dispatch) {
    if (dispatch.render_width == 0 || dispatch.render_height == 0) {
        return fail(RhiErrorCode::OutOfRange, "upscale: the render size must be greater than zero");
    }
    if (dispatch.render_width > desc.max_render_width || dispatch.render_height > desc.max_render_height) {
        return fail(RhiErrorCode::OutOfRange, "upscale: the render size is larger than the upscaler's maximum");
    }
    if (!std::isfinite(dispatch.jitter_x) || !std::isfinite(dispatch.jitter_y) || std::fabs(dispatch.jitter_x) > 0.5f ||
        std::fabs(dispatch.jitter_y) > 0.5f) {
        return fail(RhiErrorCode::OutOfRange, "upscale: the jitter must be finite and within half a pixel");
    }
    if (!(dispatch.pre_exposure > 0.0f) || !std::isfinite(dispatch.pre_exposure)) {
        return fail(RhiErrorCode::OutOfRange, "upscale: the pre-exposure must be a finite value greater than zero");
    }
    if (!(dispatch.camera_near > 0.0f) || !(dispatch.camera_far > dispatch.camera_near)) {
        return fail(RhiErrorCode::OutOfRange, "upscale: the camera planes must satisfy 0 < near < far");
    }
    if (!(dispatch.vertical_fov_radians > 0.0f) || dispatch.vertical_fov_radians >= 3.14159265f) {
        return fail(RhiErrorCode::OutOfRange, "upscale: the vertical field of view must be between zero and pi radians");
    }
    if (!(dispatch.frame_time_ms > 0.0f) || !std::isfinite(dispatch.frame_time_ms)) {
        return fail(RhiErrorCode::OutOfRange, "upscale: the frame time must be a finite value greater than zero");
    }
    return core::success();
}

RhiStatus check_upscale_texture(UpscaleRole role, const UpscalerDesc& desc, const UpscaleDispatch& dispatch, const TextureDesc& texture,
                                ResourceState state) {
    struct Rule {
        const char* name;
        ResourceState state;
        TextureUsage usage;
        Format format;
        Format alternate_format;  // Format::Unknown when there is none
    };
    const Rule rules[] = {
        {"color", ResourceState::ShaderRead, TextureUsage::ShaderResource, Format::R16G16B16A16Float, Format::Unknown},
        {"depth", ResourceState::ShaderRead, TextureUsage::ShaderResource, Format::R32Float, Format::Unknown},
        {"motion", ResourceState::ShaderRead, TextureUsage::ShaderResource, Format::R16G16B16A16Float, Format::R32G32Float},
        {"reactive", ResourceState::ShaderRead, TextureUsage::ShaderResource, Format::R32Float, Format::Unknown},
        {"exposure", ResourceState::ShaderRead, TextureUsage::ShaderResource, Format::R32Float, Format::Unknown},
        {"output", ResourceState::UnorderedAccess, TextureUsage::UnorderedAccess, Format::R16G16B16A16Float, Format::Unknown},
    };
    const Rule& rule = rules[static_cast<std::size_t>(role)];
    const std::string name = std::string("upscale ") + rule.name + ": ";
    if (state != rule.state) {
        return fail(RhiErrorCode::StateMismatch, name + "the texture must be in the state the upscale reads or writes it in");
    }
    if (!has_flags(texture.usage, rule.usage)) {
        return fail(RhiErrorCode::InvalidDescriptor, name + "the texture usage does not allow this role");
    }
    if (texture.format != rule.format && texture.format != rule.alternate_format) {
        return fail(RhiErrorCode::InvalidDescriptor, name + "the texture format does not match the upscale contract");
    }
    if (role == UpscaleRole::Output) {
        if (texture.width != desc.display_width || texture.height != desc.display_height) {
            return fail(RhiErrorCode::OutOfRange, name + "the output must be the display size");
        }
        return core::success();
    }
    if (role == UpscaleRole::Exposure) {
        return core::success();
    }
    if (texture.width < dispatch.render_width || texture.height < dispatch.render_height) {
        return fail(RhiErrorCode::OutOfRange, name + "the input is smaller than the render size");
    }
    return core::success();
}

}  // namespace engine::rhi
