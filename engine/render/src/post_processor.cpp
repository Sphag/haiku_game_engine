#include "engine/render/post_processor.hpp"

#include "engine/render/render_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace engine::render {
namespace {

using rhi::BufferDesc;
using rhi::BufferHandle;
using rhi::BufferUsage;
using rhi::Format;
using rhi::ResourceState;
using rhi::RhiErrorCode;
using rhi::RhiResult;
using rhi::RhiStatus;
using rhi::TextureDesc;
using rhi::TextureUsage;

// Parameter buffer offsets, in bytes. These are the constants of shaders/postprocess.hlsl.
constexpr std::uint64_t kParamExposure = 0;
constexpr std::uint64_t kParamBloomIntensity = 4;
constexpr std::uint64_t kParamBloomThreshold = 8;
constexpr std::uint64_t kParamBloomKnee = 12;
constexpr std::uint64_t kParamTaaWeight = 16;
constexpr std::uint64_t kParamHistoryValid = 20;
constexpr std::uint64_t kParamWidth = 24;
constexpr std::uint64_t kParamHeight = 28;
constexpr std::uint64_t kParamWeights = 32;

// Descriptor bindings of the shared layout. A binding number is also the register of its resource in the shader: t0..t5 are shader
// resources and u6, u7 storage textures, so no two bindings share a number.
constexpr std::uint32_t kBindParams = 0;
constexpr std::uint32_t kBindColor = 1;
constexpr std::uint32_t kBindVelocity = 2;
constexpr std::uint32_t kBindHistoryIn = 3;
constexpr std::uint32_t kBindBloomIn = 4;
constexpr std::uint32_t kBindOverlay = 5;
constexpr std::uint32_t kBindHdrOut = 6;
constexpr std::uint32_t kBindDisplayOut = 7;

// What each pass reads (shader-read textures) and writes (its storage texture). The parameter buffer is read by every pass.
struct PassBindings {
    std::vector<std::uint32_t> reads;
    std::uint32_t write;
};

std::vector<PassBindings> pass_bindings() {
    return {
        {{kBindColor, kBindVelocity, kBindHistoryIn}, kBindHdrOut},  // TAA resolve
        {{kBindColor}, kBindHdrOut},                                  // bloom, horizontal
        {{kBindBloomIn}, kBindHdrOut},                                // bloom, vertical
        {{kBindColor, kBindBloomIn, kBindOverlay}, kBindDisplayOut},  // tone map and overlay
    };
}

constexpr std::uint32_t kGroupSize = 8;

BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

TextureDesc texture_desc(std::uint32_t width, std::uint32_t height, Format format, TextureUsage usage, const char* name) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

void put_f32(std::vector<std::uint8_t>& bytes, std::uint64_t offset, float value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::uint64_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Rows of the overlay at the copy pitch, which a texture upload needs.
std::vector<std::uint8_t> pack_overlay_rows(const ImageRgba8& image) {
    const std::uint64_t pitch = rhi::copy_row_pitch(image.width, Format::R8G8B8A8Unorm);
    std::vector<std::uint8_t> bytes(pitch * image.height, 0);
    const std::uint64_t row = static_cast<std::uint64_t>(image.width) * 4;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::memcpy(bytes.data() + y * pitch, image.pixels.data() + y * row, row);
    }
    return bytes;
}

void destroy_into(rhi::Device& device, rhi::BufferHandle& handle, RhiStatus& first) {
    if (handle.valid()) {
        const RhiStatus status = device.destroy(handle);
        if (!status.ok() && first.ok()) {
            first = status;
        }
        handle = rhi::BufferHandle{};
    }
}

void destroy_into(rhi::Device& device, rhi::TextureHandle& handle, RhiStatus& first) {
    if (handle.valid()) {
        const RhiStatus status = device.destroy(handle);
        if (!status.ok() && first.ok()) {
            first = status;
        }
        handle = rhi::TextureHandle{};
    }
}

}  // namespace

PostProcessor::PostProcessor(rhi::Device& device, std::uint32_t width, std::uint32_t height, const PostProcessSettings& settings)
    : device_(device), width_(width), height_(height), settings_(settings), display_{}, overlay_{}, params_{}, params_upload_{} {}

RhiResult<std::unique_ptr<PostProcessor>> PostProcessor::create(rhi::Device& device, std::uint32_t width, std::uint32_t height,
                                                                const PostProcessSettings& settings, PostShaders shaders) {
    if (width == 0 || height == 0) {
        return rhi::fail<std::unique_ptr<PostProcessor>>(RhiErrorCode::OutOfRange, "post processor: the target size must be non-zero");
    }
    std::unique_ptr<PostProcessor> post(new PostProcessor(device, width, height, settings));
    if (const RhiStatus built = post->build(shaders); !built.ok()) {
        // Free what was created so far. The error that matters is the one from build().
        static_cast<void>(post->release());
        return core::failure(built.error());
    }
    return core::success(std::move(post));
}

RhiStatus PostProcessor::build(const PostShaders& shaders) {
    const std::vector<PassBindings> bindings = pass_bindings();

    const std::array<const rhi::ShaderBinary*, kPassCount> entries{&shaders.taa, &shaders.bloom_horizontal, &shaders.bloom_vertical,
                                                                  &shaders.tonemap};
    const std::array<const char*, kPassCount> names{"post taa", "post bloom horizontal", "post bloom vertical", "post tonemap"};
    for (std::size_t pass = 0; pass < kPassCount; ++pass) {
        if (entries[pass]->bytecode.empty()) {
            return rhi::fail(RhiErrorCode::InvalidDescriptor, std::string("post processor: missing shader for ") + names[pass]);
        }
        rhi::DescriptorSetLayoutDesc layout;
        layout.bindings.push_back(rhi::DescriptorBindingDesc{kBindParams, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
        for (const std::uint32_t binding : bindings[pass].reads) {
            layout.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
        }
        layout.bindings.push_back(rhi::DescriptorBindingDesc{bindings[pass].write, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute});
        RhiResult<rhi::DescriptorSetLayoutHandle> created_layout = device_.create_descriptor_set_layout(layout);
        if (!created_layout.ok()) {
            return core::failure(created_layout.error());
        }
        layouts_[pass] = std::move(created_layout).value();

        rhi::PipelineDesc pipeline;
        pipeline.kind = rhi::PipelineKind::Compute;
        pipeline.shaders = {*entries[pass]};
        pipeline.set_layouts = {layouts_[pass]};
        pipeline.debug_name = names[pass];
        RhiResult<rhi::PipelineHandle> created = device_.create_pipeline(pipeline);
        if (!created.ok()) {
            return core::failure(created.error());
        }
        pipelines_[pass] = std::move(created).value();
        RhiResult<rhi::DescriptorSetHandle> set = device_.create_descriptor_set(layouts_[pass]);
        if (!set.ok()) {
            return core::failure(set.error());
        }
        sets_[pass] = std::move(set).value();
    }

    RhiResult<BufferHandle> params = device_.create_buffer(buffer_desc(kParamBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "post params"));
    if (!params.ok()) {
        return core::failure(params.error());
    }
    params_ = std::move(params).value();
    RhiResult<BufferHandle> upload = device_.create_buffer(buffer_desc(kParamBytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "post params upload"));
    if (!upload.ok()) {
        return core::failure(upload.error());
    }
    params_upload_ = std::move(upload).value();

    if (const RhiStatus targets = build_targets(); !targets.ok()) {
        return targets;
    }
    // The overlay starts fully transparent, so the composite shows the tone-mapped scene until an overlay is set.
    ImageRgba8 clear;
    clear.width = width_;
    clear.height = height_;
    clear.pixels.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
    return upload_overlay_bytes(pack_overlay_rows(clear));
}

RhiStatus PostProcessor::build_targets() {
    const TextureUsage hdr_usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    for (rhi::TextureHandle& history : history_) {
        RhiResult<rhi::TextureHandle> created = device_.create_texture(texture_desc(width_, height_, Format::R16G16B16A16Float, hdr_usage, "taa history"));
        if (!created.ok()) {
            return core::failure(created.error());
        }
        history = std::move(created).value();
    }
    for (rhi::TextureHandle& bloom : bloom_) {
        RhiResult<rhi::TextureHandle> created = device_.create_texture(texture_desc(width_, height_, Format::R16G16B16A16Float, hdr_usage, "bloom"));
        if (!created.ok()) {
            return core::failure(created.error());
        }
        bloom = std::move(created).value();
    }
    RhiResult<rhi::TextureHandle> display = device_.create_texture(
        texture_desc(width_, height_, Format::R8G8B8A8Unorm, TextureUsage::UnorderedAccess | TextureUsage::CopySource, "post display"));
    if (!display.ok()) {
        return core::failure(display.error());
    }
    display_ = std::move(display).value();
    RhiResult<rhi::TextureHandle> overlay = device_.create_texture(
        texture_desc(width_, height_, Format::R8G8B8A8Unorm, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "post overlay"));
    if (!overlay.ok()) {
        return core::failure(overlay.error());
    }
    overlay_ = std::move(overlay).value();
    return core::success();
}

void PostProcessor::destroy_targets(RhiStatus& first) {
    for (rhi::TextureHandle& history : history_) {
        destroy_into(device_, history, first);
    }
    for (rhi::TextureHandle& bloom : bloom_) {
        destroy_into(device_, bloom, first);
    }
    destroy_into(device_, display_, first);
    destroy_into(device_, overlay_, first);
}

RhiStatus PostProcessor::resize(std::uint32_t width, std::uint32_t height) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "post processor: resize after release");
    }
    if (width == 0 || height == 0) {
        return rhi::fail(RhiErrorCode::OutOfRange, "post processor: the target size must be non-zero");
    }
    RhiStatus first = core::success();
    destroy_targets(first);
    if (!first.ok()) {
        return first;
    }
    width_ = width;
    height_ = height;
    // The history keeps its old size, so the next run sees a size change and resets (HistoryReset::Resize).
    if (const RhiStatus targets = build_targets(); !targets.ok()) {
        return targets;
    }
    ImageRgba8 clear;
    clear.width = width_;
    clear.height = height_;
    clear.pixels.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
    return upload_overlay_bytes(pack_overlay_rows(clear));
}

void PostProcessor::set_settings(const PostProcessSettings& settings) noexcept {
    const bool enabled = settings_.enabled;
    settings_ = settings;
    settings_.enabled = enabled;
}

RhiStatus PostProcessor::set_overlay(const ImageRgba8& overlay) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "post processor: set_overlay after release");
    }
    if (overlay.width != width_ || overlay.height != height_ ||
        overlay.pixels.size() != static_cast<std::size_t>(width_) * height_ * 4) {
        return rhi::fail(RhiErrorCode::OutOfRange, "post processor: the overlay must be the target size, RGBA8");
    }
    return upload_overlay_bytes(pack_overlay_rows(overlay));
}

RhiStatus PostProcessor::upload_overlay_bytes(const std::vector<std::uint8_t>& bytes) {
    RhiResult<BufferHandle> staging = device_.create_buffer(buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "overlay staging"));
    if (!staging.ok()) {
        return core::failure(staging.error());
    }
    const BufferHandle staging_buffer = std::move(staging).value();
    if (const RhiStatus uploaded = device_.upload_buffer(staging_buffer, 0, bytes.size(), bytes.data()); !uploaded.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        return uploaded;
    }
    RenderGraph graph;
    const ResourceId staging_res = graph.import_buffer("overlay staging", staging_buffer,
        buffer_desc(bytes.size(), BufferUsage::CopySource | BufferUsage::CpuWrite, "overlay staging"), ResourceState::CopySource,
        ResourceState::CopySource);
    const ResourceId overlay_res = graph.import_texture("overlay", overlay_,
        texture_desc(width_, height_, Format::R8G8B8A8Unorm, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "post overlay"),
        ResourceState::Common, ResourceState::Common);
    graph.add_pass("upload overlay", PassKind::Compute, {{staging_res, Access::CopySource}, {overlay_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_buffer_to_texture(ctx.buffer(staging_res), 0, ctx.texture(overlay_res)); });
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        static_cast<void>(device_.destroy(staging_buffer));
        return rhi::fail(RhiErrorCode::InvalidState, "post overlay: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    static_cast<void>(device_.destroy(staging_buffer));
    if (!executed.ok()) {
        return core::failure(executed.error());
    }
    return core::success();
}

RhiStatus PostProcessor::run(const PostInputs& inputs) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "post processor: run after release");
    }
    if (!inputs.color.valid() || !inputs.velocity.valid() || !inputs.readback.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "post processor: the color, velocity, and readback handles must be valid");
    }

    // The reset rule (postprocess.hpp). The motion between two camera positions decides a teleport, measured in world units.
    TaaFrameInput frame;
    frame.width = width_;
    frame.height = height_;
    frame.camera_cut = inputs.camera_cut;
    frame.teleport = has_camera_ && is_teleport(glm::vec3(0.0f), glm::vec3(inputs.camera_position - previous_camera_));
    const HistoryReset reason = taa_history_reset(has_history_, history_width_, history_height_, frame);
    last_reset_ = reason;
    const bool keep_history = reason == HistoryReset::None && settings_.taa;

    // Parameters for every pass of this run (postprocess.hlsl).
    const float exposure = exposure_from_ev100(settings_.ev100 + settings_.exposure.ev_compensation);
    const float current_weight = settings_.taa_settings.current_weight;
    const float weight = std::isfinite(current_weight) ? std::clamp(current_weight, 0.0f, 1.0f) : 1.0f;
    std::vector<std::uint8_t> params(kParamBytes, 0);
    put_f32(params, kParamExposure, exposure);
    put_f32(params, kParamBloomIntensity, std::isfinite(settings_.bloom.intensity) ? std::max(settings_.bloom.intensity, 0.0f) : 0.0f);
    put_f32(params, kParamBloomThreshold, settings_.bloom.threshold);
    put_f32(params, kParamBloomKnee, settings_.bloom.knee);
    put_f32(params, kParamTaaWeight, weight);
    put_u32(params, kParamHistoryValid, keep_history ? 1u : 0u);
    put_u32(params, kParamWidth, width_);
    put_u32(params, kParamHeight, height_);
    const std::vector<float> weights = bloom_blur_weights(kBloomRadius);
    for (std::size_t i = 0; i < weights.size(); ++i) {
        put_f32(params, kParamWeights + 4 * i, weights[i]);
    }
    if (const RhiStatus uploaded = device_.upload_buffer(params_upload_, 0, kParamBytes, params.data()); !uploaded.ok()) {
        return uploaded;
    }

    // Descriptors name this run's inputs. The sets are not in flight: every run waits for its submission before it returns.
    const std::size_t out = history_out_;
    const std::size_t in = 1 - history_out_;
    const auto bind = [this](std::size_t pass, std::uint32_t binding, rhi::TextureHandle texture) -> RhiStatus {
        return device_.write_texture(sets_[pass], binding, 0, texture);
    };
    for (std::size_t pass = 0; pass < kPassCount; ++pass) {
        if (const RhiStatus bound = device_.write_buffer(sets_[pass], kBindParams, 0, params_); !bound.ok()) {
            return bound;
        }
    }
    if (const RhiStatus bound = bind(kPassTaa, kBindColor, inputs.color); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassTaa, kBindVelocity, inputs.velocity); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassTaa, kBindHistoryIn, history_[in]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassTaa, kBindHdrOut, history_[out]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassBloomHorizontal, kBindColor, history_[out]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassBloomHorizontal, kBindHdrOut, bloom_[0]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassBloomVertical, kBindBloomIn, bloom_[0]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassBloomVertical, kBindHdrOut, bloom_[1]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassTonemap, kBindColor, history_[out]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassTonemap, kBindBloomIn, bloom_[1]); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassTonemap, kBindOverlay, overlay_); !bound.ok()) {
        return bound;
    }
    if (const RhiStatus bound = bind(kPassTonemap, kBindDisplayOut, display_); !bound.ok()) {
        return bound;
    }

    RenderGraph graph;
    const TextureUsage hdr_usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    const ResourceId color_res = graph.import_texture("scene color",
        inputs.color, texture_desc(width_, height_, Format::R16G16B16A16Float, TextureUsage::ShaderResource, "scene color"),
        ResourceState::Common, ResourceState::Common);
    const ResourceId velocity_res = graph.import_texture("velocity", inputs.velocity,
        texture_desc(width_, height_, Format::R32G32Float, TextureUsage::ShaderResource, "scene velocity"), ResourceState::Common,
        ResourceState::Common);
    const ResourceId history_in_res = graph.import_texture("history in", history_[in], texture_desc(width_, height_, Format::R16G16B16A16Float,
        hdr_usage, "taa history"), ResourceState::Common, ResourceState::Common);
    const ResourceId history_out_res = graph.import_texture("history out", history_[out],
        texture_desc(width_, height_, Format::R16G16B16A16Float, hdr_usage, "taa history"), ResourceState::Common, ResourceState::Common);
    const ResourceId bloom_a_res = graph.import_texture("bloom a", bloom_[0], texture_desc(width_, height_, Format::R16G16B16A16Float, hdr_usage, "bloom"),
        ResourceState::Common, ResourceState::Common);
    const ResourceId bloom_b_res = graph.import_texture("bloom b", bloom_[1], texture_desc(width_, height_, Format::R16G16B16A16Float, hdr_usage, "bloom"),
        ResourceState::Common, ResourceState::Common);
    const ResourceId overlay_res = graph.import_texture("overlay", overlay_,
        texture_desc(width_, height_, Format::R8G8B8A8Unorm, TextureUsage::ShaderResource | TextureUsage::CopyDestination, "post overlay"),
        ResourceState::Common, ResourceState::Common);
    const ResourceId display_res = graph.import_texture("display", display_,
        texture_desc(width_, height_, Format::R8G8B8A8Unorm, TextureUsage::UnorderedAccess | TextureUsage::CopySource, "post display"),
        ResourceState::Common, ResourceState::Common);
    const ResourceId readback_res = graph.import_buffer("display readback", inputs.readback,
        buffer_desc(rhi::copy_row_pitch(width_, Format::R8G8B8A8Unorm) * height_, BufferUsage::CpuRead | BufferUsage::CopyDestination,
                    "display readback"),
        ResourceState::CopyDestination, ResourceState::CopyDestination);
    const ResourceId params_upload_res = graph.import_buffer("post params upload", params_upload_,
        buffer_desc(kParamBytes, BufferUsage::CopySource | BufferUsage::CpuWrite, "post params upload"), ResourceState::CopySource,
        ResourceState::CopySource);
    const ResourceId params_res = graph.import_buffer("post params", params_,
        buffer_desc(kParamBytes, BufferUsage::ShaderResource | BufferUsage::CopyDestination, "post params"), ResourceState::Common,
        ResourceState::Common);

    const std::uint32_t groups_x = (width_ + kGroupSize - 1) / kGroupSize;
    const std::uint32_t groups_y = (height_ + kGroupSize - 1) / kGroupSize;
    const auto dispatch_pass = [this, groups_x, groups_y](std::size_t pass) {
        return [this, pass, groups_x, groups_y](PassContext& ctx) -> RhiStatus {
            rhi::CommandList& commands = ctx.commands();
            if (const RhiStatus set = commands.set_pipeline(pipelines_[pass]); !set.ok()) {
                return set;
            }
            if (const RhiStatus set = commands.set_descriptor_set(0, sets_[pass]); !set.ok()) {
                return set;
            }
            return commands.dispatch(groups_x, groups_y, 1);
        };
    };

    graph.add_pass("post params", PassKind::Compute, {{params_upload_res, Access::CopySource}, {params_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_buffer(ctx.buffer(params_upload_res), 0, ctx.buffer(params_res), 0, kParamBytes); });
    graph.add_pass("taa resolve", PassKind::Compute,
                   {{color_res, Access::ShaderRead}, {velocity_res, Access::ShaderRead}, {history_in_res, Access::ShaderRead},
                    {history_out_res, Access::StorageWrite}, {params_res, Access::ShaderRead}},
                   dispatch_pass(kPassTaa));
    graph.add_pass("bloom horizontal", PassKind::Compute,
                   {{history_out_res, Access::ShaderRead}, {bloom_a_res, Access::StorageWrite}, {params_res, Access::ShaderRead}},
                   dispatch_pass(kPassBloomHorizontal));
    graph.add_pass("bloom vertical", PassKind::Compute,
                   {{bloom_a_res, Access::ShaderRead}, {bloom_b_res, Access::StorageWrite}, {params_res, Access::ShaderRead}},
                   dispatch_pass(kPassBloomVertical));
    graph.add_pass("tone map", PassKind::Compute,
                   {{history_out_res, Access::ShaderRead}, {bloom_b_res, Access::ShaderRead}, {overlay_res, Access::ShaderRead},
                    {display_res, Access::StorageWrite}, {params_res, Access::ShaderRead}},
                   dispatch_pass(kPassTonemap));
    graph.add_pass("read back display", PassKind::Compute, {{display_res, Access::CopySource}, {readback_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_texture_to_buffer(ctx.texture(display_res), ctx.buffer(readback_res), 0); });

    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        return rhi::fail(RhiErrorCode::InvalidState, "post processor: " + compiled.errors.front().message);
    }
    const RhiResult<ExecutionReport> executed = execute(device_, *compiled.graph);
    if (!executed.ok()) {
        // The history may be half written, so the next run starts over.
        has_history_ = false;
        return core::failure(executed.error());
    }
    has_history_ = true;
    history_width_ = width_;
    history_height_ = height_;
    previous_camera_ = inputs.camera_position;
    has_camera_ = true;
    history_out_ = 1 - history_out_;
    return core::success();
}

RhiStatus PostProcessor::release() {
    if (released_) {
        return core::success();
    }
    released_ = true;
    RhiStatus first = core::success();
    for (rhi::DescriptorSetHandle& set : sets_) {
        if (set.valid()) {
            const RhiStatus status = device_.destroy(set);
            if (!status.ok() && first.ok()) {
                first = status;
            }
            set = rhi::DescriptorSetHandle{};
        }
    }
    for (rhi::PipelineHandle& pipeline : pipelines_) {
        if (pipeline.valid()) {
            const RhiStatus status = device_.destroy(pipeline);
            if (!status.ok() && first.ok()) {
                first = status;
            }
            pipeline = rhi::PipelineHandle{};
        }
    }
    for (rhi::DescriptorSetLayoutHandle& layout : layouts_) {
        if (layout.valid()) {
            const RhiStatus status = device_.destroy(layout);
            if (!status.ok() && first.ok()) {
                first = status;
            }
            layout = rhi::DescriptorSetLayoutHandle{};
        }
    }
    destroy_targets(first);
    destroy_into(device_, params_upload_, first);
    destroy_into(device_, params_, first);
    return first;
}

}  // namespace engine::render
