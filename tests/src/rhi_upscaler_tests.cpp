// The upscale contract of the RHI (US-046). The dispatch and texture rules are checked on the CPU, so they run on every operating system.
// The CPU reference device runs no FSR 2 upscale: it reports Unsupported, and the command list keeps the same error.

#include "engine/rhi/error.hpp"
#include "engine/rhi/upscaler.hpp"
#include "engine/rhi/validation_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using engine::rhi::check_upscale_texture;
using engine::rhi::Format;
using engine::rhi::ResourceState;
using engine::rhi::RhiErrorCode;
using engine::rhi::TextureDesc;
using engine::rhi::TextureUsage;
using engine::rhi::UpscaleDispatch;
using engine::rhi::UpscaleRole;
using engine::rhi::Upscaler;
using engine::rhi::UpscalerDesc;
using engine::rhi::validate_upscale_dispatch;
using engine::rhi::ValidationDevice;

namespace {

UpscalerDesc full_profile_desc() {
    UpscalerDesc desc;
    desc.max_render_width = 1280;
    desc.max_render_height = 720;
    desc.display_width = 1920;
    desc.display_height = 1080;
    return desc;
}

UpscaleDispatch full_profile_dispatch() {
    UpscaleDispatch dispatch;
    dispatch.render_width = 1280;
    dispatch.render_height = 720;
    dispatch.jitter_x = 0.25f;
    dispatch.jitter_y = -0.125f;
    return dispatch;
}

TextureDesc texture(std::uint32_t width, std::uint32_t height, Format format, TextureUsage usage) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = usage;
    return desc;
}

// The engine has no DX12 or Vulkan type in its public headers, so a test upscaler only needs the description.
class FakeUpscaler final : public Upscaler {
public:
    explicit FakeUpscaler(UpscalerDesc desc) : desc_(desc) {}
    [[nodiscard]] const UpscalerDesc& desc() const noexcept override {
        return desc_;
    }

private:
    UpscalerDesc desc_;
};

}  // namespace

TEST_CASE("An upscale dispatch is validated against its upscaler and the camera", "[unit]") {
    const UpscalerDesc desc = full_profile_desc();
    REQUIRE(validate_upscale_dispatch(desc, full_profile_dispatch()).ok());

    UpscaleDispatch zero = full_profile_dispatch();
    zero.render_width = 0;
    REQUIRE(validate_upscale_dispatch(desc, zero).error().code == RhiErrorCode::OutOfRange);

    UpscaleDispatch too_large = full_profile_dispatch();
    too_large.render_height = 1080;
    REQUIRE(validate_upscale_dispatch(desc, too_large).error().code == RhiErrorCode::OutOfRange);

    UpscaleDispatch wide_jitter = full_profile_dispatch();
    wide_jitter.jitter_x = 0.6f;
    REQUIRE(validate_upscale_dispatch(desc, wide_jitter).error().code == RhiErrorCode::OutOfRange);

    UpscaleDispatch nan_jitter = full_profile_dispatch();
    nan_jitter.jitter_y = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(validate_upscale_dispatch(desc, nan_jitter).error().code == RhiErrorCode::OutOfRange);

    UpscaleDispatch no_exposure = full_profile_dispatch();
    no_exposure.pre_exposure = 0.0f;
    REQUIRE(validate_upscale_dispatch(desc, no_exposure).error().code == RhiErrorCode::OutOfRange);

    UpscaleDispatch inverted_planes = full_profile_dispatch();
    inverted_planes.camera_near = 10.0f;
    inverted_planes.camera_far = 1.0f;
    REQUIRE(validate_upscale_dispatch(desc, inverted_planes).error().code == RhiErrorCode::OutOfRange);

    UpscaleDispatch flat_fov = full_profile_dispatch();
    flat_fov.vertical_fov_radians = 0.0f;
    REQUIRE(validate_upscale_dispatch(desc, flat_fov).error().code == RhiErrorCode::OutOfRange);
}

TEST_CASE("Each upscale input must be in its state, usage, format, and size", "[unit]") {
    const UpscalerDesc desc = full_profile_desc();
    const UpscaleDispatch dispatch = full_profile_dispatch();

    const TextureDesc color = texture(1280, 720, Format::R16G16B16A16Float, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Color, desc, dispatch, color, ResourceState::ShaderRead).ok());
    REQUIRE(check_upscale_texture(UpscaleRole::Color, desc, dispatch, color, ResourceState::Common).error().code ==
            RhiErrorCode::StateMismatch);

    const TextureDesc untagged = texture(1280, 720, Format::R16G16B16A16Float, TextureUsage::RenderTarget);
    REQUIRE(check_upscale_texture(UpscaleRole::Color, desc, dispatch, untagged, ResourceState::ShaderRead).error().code ==
            RhiErrorCode::InvalidDescriptor);

    const TextureDesc narrow = texture(1280, 720, Format::R8G8B8A8Unorm, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Color, desc, dispatch, narrow, ResourceState::ShaderRead).error().code ==
            RhiErrorCode::InvalidDescriptor);

    const TextureDesc short_color = texture(1280, 719, Format::R16G16B16A16Float, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Color, desc, dispatch, short_color, ResourceState::ShaderRead).error().code ==
            RhiErrorCode::OutOfRange);

    // Inputs may be larger than the render size: FSR 2 reads only the render region.
    const TextureDesc large_depth = texture(1920, 1080, Format::R32Float, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Depth, desc, dispatch, large_depth, ResourceState::ShaderRead).ok());

    // The motion vectors may be the scene's R32G32 velocity target or the denoiser's RGBA16F motion.
    const TextureDesc velocity = texture(1280, 720, Format::R32G32Float, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Motion, desc, dispatch, velocity, ResourceState::ShaderRead).ok());
    const TextureDesc motion = texture(1280, 720, Format::R16G16B16A16Float, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Motion, desc, dispatch, motion, ResourceState::ShaderRead).ok());

    // A one-texel exposure needs no render size; the reactive mask does.
    const TextureDesc exposure = texture(1, 1, Format::R32Float, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Exposure, desc, dispatch, exposure, ResourceState::ShaderRead).ok());
    const TextureDesc reactive = texture(640, 360, Format::R32Float, TextureUsage::ShaderResource);
    REQUIRE(check_upscale_texture(UpscaleRole::Reactive, desc, dispatch, reactive, ResourceState::ShaderRead).error().code ==
            RhiErrorCode::OutOfRange);

    const TextureDesc output = texture(1920, 1080, Format::R16G16B16A16Float, TextureUsage::UnorderedAccess);
    REQUIRE(check_upscale_texture(UpscaleRole::Output, desc, dispatch, output, ResourceState::UnorderedAccess).ok());
    REQUIRE(check_upscale_texture(UpscaleRole::Output, desc, dispatch, output, ResourceState::ShaderRead).error().code ==
            RhiErrorCode::StateMismatch);
    const TextureDesc render_sized = texture(1280, 720, Format::R16G16B16A16Float, TextureUsage::UnorderedAccess);
    REQUIRE(check_upscale_texture(UpscaleRole::Output, desc, dispatch, render_sized, ResourceState::UnorderedAccess).error().code ==
            RhiErrorCode::OutOfRange);
}

TEST_CASE("The CPU reference device runs no FSR 2 upscale and its lists refuse one", "[unit]") {
    ValidationDevice device;

    const auto created = device.create_upscaler(full_profile_desc());
    REQUIRE_FALSE(created.ok());
    REQUIRE(created.error().code == RhiErrorCode::Unsupported);

    FakeUpscaler upscaler(full_profile_desc());
    auto list = device.create_command_list(engine::rhi::QueueType::Graphics);
    REQUIRE(list.ok());
    REQUIRE(list.value()->upscale(upscaler, full_profile_dispatch()).error().code == RhiErrorCode::Unsupported);

    REQUIRE(list.value()->close().ok());
    REQUIRE(list.value()->upscale(upscaler, full_profile_dispatch()).error().code == RhiErrorCode::InvalidState);
}
