#include "engine/render/denoiser.hpp"

#include "engine/core/result.hpp"
#include "engine/render/render_graph.hpp"
#include "gpu_upload.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine::render {

namespace {

using rhi::BufferHandle;
using rhi::BufferUsage;
using rhi::Format;
using rhi::ResourceState;
using rhi::RhiErrorCode;
using rhi::RhiResult;
using rhi::RhiStatus;
using rhi::TextureDesc;

// Byte offsets of the parameter block (shaders/denoise.hlsl).
constexpr std::uint32_t kParamWidth = 0;
constexpr std::uint32_t kParamHeight = 4;
constexpr std::uint32_t kParamFlags = 8;
constexpr std::uint32_t kParamMaxAge = 12;
constexpr std::uint32_t kParamHistoryWeight = 16;
constexpr std::uint32_t kParamDepthTolerance = 20;
constexpr std::uint32_t kParamNormalThreshold = 24;
constexpr std::uint32_t kParamSpatialRadius = 28;
constexpr std::uint32_t kFlagReset = 1;
constexpr std::uint32_t kFlagHistoryValid = 2;

constexpr BufferUsage kBufferUsage = BufferUsage::ShaderResource | BufferUsage::CopyDestination;

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

bool finite(const glm::vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

// A non-finite sample contributes nothing: the same rule the shader applies to its loads.
glm::vec4 sanitized(const glm::vec4& value) {
    return finite(value) ? value : glm::vec4(0.0f);
}

// Whether a neighbour's geometry agrees with the pixel's: the linear depth within the relative tolerance, and the normals close.
bool geometry_agrees(float depth_a, const glm::vec3& normal_a, float depth_b, const glm::vec3& normal_b, const DenoiseSettings& settings) {
    return std::fabs(depth_a - depth_b) <= settings.depth_tolerance * depth_b && glm::dot(normal_a, normal_b) >= settings.normal_threshold;
}

}  // namespace

std::optional<std::string> validate_denoise_settings(const DenoiseSettings& settings) {
    if (!std::isfinite(settings.history_weight) || settings.history_weight < 0.0f || settings.history_weight >= 1.0f) {
        return "history weight must be in [0, 1)";
    }
    if (settings.max_age < 1) {
        return "max age must be at least 1";
    }
    if (!std::isfinite(settings.depth_tolerance) || settings.depth_tolerance < 0.0f) {
        return "depth tolerance must be finite and non-negative";
    }
    if (!std::isfinite(settings.normal_threshold) || settings.normal_threshold < -1.0f || settings.normal_threshold > 1.0f) {
        return "normal threshold must be in [-1, 1]";
    }
    if (settings.spatial_radius > kDenoiseMaxRadius) {
        return "spatial radius must be at most " + std::to_string(kDenoiseMaxRadius);
    }
    return std::nullopt;
}

bool camera_cut(const CameraPose& previous, const CameraPose& current, float max_translation, float max_angle_degrees) {
    const float moved = glm::length(current.eye - previous.eye);
    const float cosine = std::clamp(glm::dot(glm::normalize(previous.forward), glm::normalize(current.forward)), -1.0f, 1.0f);
    const float turned = std::acos(cosine) * 180.0f / 3.14159265358979323846f;
    return moved > max_translation || turned > max_angle_degrees;
}

bool light_changed(const glm::vec3& previous_direction, const glm::vec3& direction, float max_angle_degrees) {
    const float cosine = std::clamp(glm::dot(glm::normalize(previous_direction), glm::normalize(direction)), -1.0f, 1.0f);
    return std::acos(cosine) * 180.0f / 3.14159265358979323846f > max_angle_degrees;
}

DenoiseReference denoise_reference(const DenoiseTarget& target, const DenoiseHistory& previous, const DenoiseSettings& settings, bool reset) {
    const std::uint32_t width = target.width;
    const std::uint32_t height = target.height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    DenoiseReference result;
    result.output.assign(count, glm::vec4(0.0f));
    result.accepted.assign(count, 0);
    result.history.valid = true;
    result.history.width = width;
    result.history.height = height;
    result.history.value.assign(count, glm::vec4(0.0f));
    result.history.geometry.assign(count, DenoiseGeometry{});

    const bool use_history = !reset && previous.valid && previous.width == width && previous.height == height;
    const int radius = static_cast<int>(settings.spatial_radius);
    const float width_f = static_cast<float>(width);
    const float height_f = static_cast<float>(height);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = target.index(x, y);
            const float depth = target.depth[i];
            const glm::vec3 normal = target.normal[i];

            // The depth- and normal-aware box prefilter. The pixel itself always passes, so the sum has at least one term.
            glm::vec4 sum(0.0f);
            float weight = 0.0f;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const std::uint32_t qx = static_cast<std::uint32_t>(std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(width) - 1));
                    const std::uint32_t qy = static_cast<std::uint32_t>(std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(height) - 1));
                    const std::size_t j = target.index(qx, qy);
                    if (!geometry_agrees(target.depth[j], target.normal[j], depth, normal, settings)) {
                        continue;
                    }
                    sum += sanitized(target.value[j]);
                    weight += 1.0f;
                }
            }
            const glm::vec4 filtered = sum / weight;

            // Reprojection: the pixel's previous position, and the history sample there if its geometry still agrees.
            const glm::vec2 uv((static_cast<float>(x) + 0.5f) / width_f, (static_cast<float>(y) + 0.5f) / height_f);
            const glm::vec2 previous_uv = uv + target.motion[i];
            const int px = static_cast<int>(std::floor(previous_uv.x * width_f));
            const int py = static_cast<int>(std::floor(previous_uv.y * height_f));
            bool accepted = false;
            std::size_t j = 0;
            if (use_history && px >= 0 && py >= 0 && px < static_cast<int>(width) && py < static_cast<int>(height)) {
                j = previous.index(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                const DenoiseGeometry& prior = previous.geometry[j];
                accepted = finite(previous.value[j]) && geometry_agrees(prior.depth, prior.normal, depth, normal, settings);
            }

            glm::vec4 value = filtered;
            float age = 1.0f;
            if (accepted) {
                age = std::min(previous.geometry[j].age + 1.0f, static_cast<float>(settings.max_age));
                const float alpha = std::max(1.0f - settings.history_weight, 1.0f / (age + 1.0f));
                value = glm::mix(previous.value[j], filtered, alpha);
            }
            result.output[i] = value;
            result.accepted[i] = accepted ? 1 : 0;
            result.history.value[i] = value;
            result.history.geometry[i] = DenoiseGeometry{depth, normal, age};
        }
    }
    return result;
}

std::vector<std::uint8_t> pack_denoise_parameters(const DenoiseSettings& settings, std::uint32_t width, std::uint32_t height, bool reset,
                                                  bool history_valid) {
    std::vector<std::uint8_t> bytes(kDenoiseParameterBytes, 0);
    put(bytes, kParamWidth, width);
    put(bytes, kParamHeight, height);
    put(bytes, kParamFlags, (reset ? kFlagReset : 0u) | (history_valid ? kFlagHistoryValid : 0u));
    put(bytes, kParamMaxAge, settings.max_age);
    put(bytes, kParamHistoryWeight, settings.history_weight);
    put(bytes, kParamDepthTolerance, settings.depth_tolerance);
    put(bytes, kParamNormalThreshold, settings.normal_threshold);
    put(bytes, kParamSpatialRadius, settings.spatial_radius);
    return bytes;
}

RhiResult<std::unique_ptr<TemporalDenoiser>> TemporalDenoiser::create(rhi::Device& device, std::uint32_t width, std::uint32_t height,
                                                                      rhi::ShaderBinary shader) {
    if (width == 0 || height == 0) {
        return rhi::fail<std::unique_ptr<TemporalDenoiser>>(RhiErrorCode::OutOfRange, "the denoiser needs a non-zero size");
    }
    if (shader.bytecode.empty()) {
        return rhi::fail<std::unique_ptr<TemporalDenoiser>>(RhiErrorCode::InvalidState, "the denoiser needs its compute shader (CSTemporalDenoise)");
    }

    // t0 parameters, t1 to t4 the inputs, t5 to t7 the previous history, and u8 to u10 the next history. The inputs and the previous history
    // are read, so they are ShaderResource; the next history is written.
    rhi::DescriptorSetLayoutDesc layout_desc;
    for (std::uint32_t binding = 0; binding <= 7; ++binding) {
        layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
    }
    for (std::uint32_t binding = 8; binding <= 10; ++binding) {
        layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute});
    }
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device.create_descriptor_set_layout(layout_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }

    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.kind = rhi::PipelineKind::Compute;
    pipeline_desc.shaders = {std::move(shader)};
    pipeline_desc.set_layouts = {layout.value()};
    pipeline_desc.debug_name = "temporal denoiser";
    RhiResult<rhi::PipelineHandle> pipeline = device.create_pipeline(pipeline_desc);
    if (!pipeline.ok()) {
        return core::failure(pipeline.error());
    }

    std::unique_ptr<TemporalDenoiser> denoiser(new TemporalDenoiser(device, width, height));
    denoiser->layout_ = layout.value();
    denoiser->pipeline_ = pipeline.value();

    RhiResult<BufferHandle> parameters = device.create_buffer(detail::gpu_buffer_desc(kDenoiseParameterBytes, kBufferUsage, "denoiser parameters"));
    if (!parameters.ok()) {
        return core::failure(parameters.error());
    }
    denoiser->parameters_ = parameters.value();

    // The history textures. The value keeps the denoised rgba, the depth the linear view depth, and the normal the world normal with the
    // age in w. The age is a float so that it can count up to max_age exactly.
    for (int parity = 0; parity < 2; ++parity) {
        TextureDesc desc;
        desc.width = width;
        desc.height = height;
        // The value history is the output, so it can also be copied out for a readback.
        desc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopySource;
        desc.format = Format::R16G16B16A16Float;
        desc.debug_name = "denoiser value history";
        RhiResult<rhi::TextureHandle> value = device.create_texture(desc);
        if (!value.ok()) {
            return core::failure(value.error());
        }
        desc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
        desc.format = Format::R32Float;
        desc.debug_name = "denoiser depth history";
        RhiResult<rhi::TextureHandle> depth = device.create_texture(desc);
        if (!depth.ok()) {
            return core::failure(depth.error());
        }
        desc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
        desc.format = Format::R16G16B16A16Float;
        desc.debug_name = "denoiser normal history";
        RhiResult<rhi::TextureHandle> normal = device.create_texture(desc);
        if (!normal.ok()) {
            return core::failure(normal.error());
        }
        denoiser->value_[parity] = value.value();
        denoiser->depth_[parity] = depth.value();
        denoiser->normal_[parity] = normal.value();
    }

    // Set p reads history p and writes history 1 - p. The bindings that do not change are written here; set_inputs writes the rest.
    for (int parity = 0; parity < 2; ++parity) {
        RhiResult<rhi::DescriptorSetHandle> set = device.create_descriptor_set(layout.value());
        if (!set.ok()) {
            return core::failure(set.error());
        }
        denoiser->sets_[parity] = set.value();
        const int next = 1 - parity;
        if (const RhiStatus written = device.write_buffer(set.value(), 0, 0, denoiser->parameters_); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_texture(set.value(), 5, 0, denoiser->value_[parity]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_texture(set.value(), 6, 0, denoiser->depth_[parity]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_texture(set.value(), 7, 0, denoiser->normal_[parity]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_texture(set.value(), 8, 0, denoiser->value_[next]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_texture(set.value(), 9, 0, denoiser->depth_[next]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_texture(set.value(), 10, 0, denoiser->normal_[next]); !written.ok()) {
            return core::failure(written.error());
        }
    }
    return core::success(std::move(denoiser));
}

TemporalDenoiser::TemporalDenoiser(rhi::Device& device, std::uint32_t width, std::uint32_t height) noexcept
    : device_(device), width_(width), height_(height) {}

RhiStatus TemporalDenoiser::set_inputs(const DenoiseInputs& inputs) {
    if (!inputs.value.valid() || !inputs.depth.valid() || !inputs.normal.valid() || !inputs.motion.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "the denoiser needs its value, depth, normal, and motion inputs");
    }
    for (const rhi::DescriptorSetHandle set : sets_) {
        if (const RhiStatus written = device_.write_texture(set, 1, 0, inputs.value); !written.ok()) {
            return written;
        }
        if (const RhiStatus written = device_.write_texture(set, 2, 0, inputs.depth); !written.ok()) {
            return written;
        }
        if (const RhiStatus written = device_.write_texture(set, 3, 0, inputs.normal); !written.ok()) {
            return written;
        }
        if (const RhiStatus written = device_.write_texture(set, 4, 0, inputs.motion); !written.ok()) {
            return written;
        }
    }
    inputs_set_ = true;
    return core::success();
}

RhiStatus TemporalDenoiser::update_frame(const DenoiseSettings& settings, bool reset) {
    if (const std::optional<std::string> problem = validate_denoise_settings(settings)) {
        return rhi::fail(RhiErrorCode::OutOfRange, "denoiser settings: " + *problem);
    }
    const std::vector<std::uint8_t> bytes = pack_denoise_parameters(settings, width_, height_, reset, history_valid_);
    const ResourceState before = parameters_uploaded_ ? ResourceState::ShaderRead : ResourceState::Common;
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, parameters_, kBufferUsage, kDenoiseParameterBytes, bytes, before);
        !uploaded.ok()) {
        return uploaded;
    }
    parameters_uploaded_ = true;
    frame_set_ = true;
    return core::success();
}

RhiStatus TemporalDenoiser::record(rhi::CommandList& list) {
    if (!inputs_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "temporal denoiser: set_inputs before record");
    }
    if (!frame_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "temporal denoiser: update_frame before record");
    }
    const std::uint32_t previous = parity_;
    const std::uint32_t next = 1 - parity_;

    // The previous history is read and the next one is written. A texture that has never been written is still Common.
    const auto move = [&list](rhi::TextureHandle texture, ResourceState& state, ResourceState target) -> RhiStatus {
        if (state == target) {
            return core::success();
        }
        RhiStatus status = list.transition(texture, state, target);
        if (status.ok()) {
            state = target;
        }
        return status;
    };
    if (const RhiStatus status = move(value_[previous], value_state_[previous], ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(depth_[previous], depth_state_[previous], ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(normal_[previous], normal_state_[previous], ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(value_[next], value_state_[next], ResourceState::UnorderedAccess); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(depth_[next], depth_state_[next], ResourceState::UnorderedAccess); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(normal_[next], normal_state_[next], ResourceState::UnorderedAccess); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_pipeline(pipeline_); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_descriptor_set(0, sets_[previous]); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(value_[next], value_state_[next], ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(depth_[next], depth_state_[next], ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = move(normal_[next], normal_state_[next], ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    parity_ = next;
    history_valid_ = true;
    return core::success();
}

rhi::TextureHandle TemporalDenoiser::output() const noexcept {
    return history_valid_ ? value_[parity_] : rhi::TextureHandle{};
}

RhiStatus TemporalDenoiser::release() {
    RhiStatus first = core::success();
    const auto keep = [&first](RhiStatus status) {
        if (!status.ok() && first.ok()) {
            first = std::move(status);
        }
    };
    keep(device_.destroy(sets_[0]));
    keep(device_.destroy(sets_[1]));
    keep(device_.destroy(layout_));
    keep(device_.destroy(pipeline_));
    keep(device_.destroy(parameters_));
    for (int parity = 0; parity < 2; ++parity) {
        keep(device_.destroy(value_[parity]));
        keep(device_.destroy(depth_[parity]));
        keep(device_.destroy(normal_[parity]));
    }
    inputs_set_ = false;
    frame_set_ = false;
    history_valid_ = false;
    return first;
}

}  // namespace engine::render
