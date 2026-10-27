#include "engine/render/volumetric_fog.hpp"

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
using rhi::ResourceState;
using rhi::RhiErrorCode;
using rhi::RhiResult;
using rhi::RhiStatus;

// Byte offsets of the parameter block (shaders/volumetric_fog.hlsl).
constexpr std::uint32_t kParamEye = 0;
constexpr std::uint32_t kParamForward = 16;
constexpr std::uint32_t kParamRight = 32;
constexpr std::uint32_t kParamUp = 48;
constexpr std::uint32_t kParamPreviousEye = 64;
constexpr std::uint32_t kParamPreviousForward = 80;
constexpr std::uint32_t kParamPreviousRight = 96;
constexpr std::uint32_t kParamPreviousUp = 112;
constexpr std::uint32_t kParamSunDirection = 128;
constexpr std::uint32_t kParamSunRadiance = 144;
constexpr std::uint32_t kParamAlbedo = 160;
constexpr std::uint32_t kParamDensity = 172;
constexpr std::uint32_t kParamHeightFalloff = 176;
constexpr std::uint32_t kParamAnisotropy = 180;
constexpr std::uint32_t kParamNear = 184;
constexpr std::uint32_t kParamFar = 188;
constexpr std::uint32_t kParamWidth = 192;
constexpr std::uint32_t kParamHeight = 196;
constexpr std::uint32_t kParamDepth = 200;
constexpr std::uint32_t kParamFlags = 204;
constexpr std::uint32_t kParamHistoryWeight = 208;
constexpr std::uint32_t kParamMaxAge = 212;
constexpr std::uint32_t kParamLightCount = 216;
constexpr std::uint32_t kParamLights = 224;
constexpr std::uint32_t kLightStride = 32;
constexpr std::uint32_t kLightRange = 12;
constexpr std::uint32_t kLightRadiance = 16;
constexpr std::uint32_t kLightShadowed = 28;
// The camera's depth range, after the lights (the light block ends at 224 + 16 * 32 = 736).
constexpr std::uint32_t kParamCameraNear = 736;
constexpr std::uint32_t kParamCameraFar = 740;
constexpr std::uint32_t kFlagReset = 1;
constexpr std::uint32_t kFlagHistoryValid = 2;
constexpr std::uint32_t kFlagShadows = 4;

constexpr float kPi = 3.14159265358979323846f;
constexpr BufferUsage kParameterUsage = BufferUsage::ShaderResource | BufferUsage::CopyDestination;
constexpr BufferUsage kGridUsage = BufferUsage::ShaderResource | BufferUsage::UnorderedAccess;

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const glm::vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

// The window that brings a light's contribution to zero at its range.
float range_window(float distance, float range) {
    const float t = std::clamp(distance / range, 0.0f, 1.0f);
    return (1.0f - t) * (1.0f - t);
}

// The reprojection of a froxel centre into the previous frame: the previous grid's column, row, and slice, or false when the centre was
// outside the previous frustum.
bool previous_froxel(const FogFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t depth, const glm::vec3& point,
                     std::uint32_t& x, std::uint32_t& y, std::uint32_t& z) {
    const glm::vec3 offset = point - frame.previous_eye;
    const float view_depth = glm::dot(offset, frame.previous_forward);
    if (!(view_depth > 0.0f)) {
        return false;
    }
    const float ndc_x = glm::dot(offset, frame.previous_right) / (view_depth * glm::dot(frame.previous_right, frame.previous_right));
    const float ndc_y = glm::dot(offset, frame.previous_up) / (view_depth * glm::dot(frame.previous_up, frame.previous_up));
    if (std::fabs(ndc_x) > 1.0f || std::fabs(ndc_y) > 1.0f) {
        return false;
    }
    const float slice = static_cast<float>(depth) * std::log(view_depth / frame.near_plane) / std::log(frame.far_plane / frame.near_plane);
    if (!(slice >= 0.0f) || slice >= static_cast<float>(depth)) {
        return false;
    }
    x = std::min(static_cast<std::uint32_t>((ndc_x + 1.0f) * 0.5f * static_cast<float>(width)), width - 1);
    y = std::min(static_cast<std::uint32_t>((1.0f - ndc_y) * 0.5f * static_cast<float>(height)), height - 1);
    z = static_cast<std::uint32_t>(slice);
    return true;
}

}  // namespace

std::optional<std::string> validate_fog(const FogFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    if (width == 0 || height == 0 || depth == 0) {
        return "the fog grid needs a non-zero size";
    }
    const bool camera_finite = finite(frame.eye) && finite(frame.forward) && finite(frame.right) && finite(frame.up) &&
                               finite(frame.previous_eye) && finite(frame.previous_forward) && finite(frame.previous_right) &&
                               finite(frame.previous_up);
    if (!camera_finite) {
        return "the camera must be finite";
    }
    const auto zero = [](const glm::vec3& v) { return glm::dot(v, v) == 0.0f; };
    if (zero(frame.right) || zero(frame.up) || zero(frame.previous_right) || zero(frame.previous_up)) {
        return "the camera's half extents must be non-zero";
    }
    if (!(frame.near_plane > 0.0f) || !(frame.far_plane > frame.near_plane) || !std::isfinite(frame.far_plane)) {
        return "the slices need 0 < near < far";
    }
    if (!(frame.camera_near > 0.0f) || !(frame.camera_far > frame.camera_near) || !std::isfinite(frame.camera_far)) {
        return "the camera range needs 0 < near < far";
    }
    if (!std::isfinite(frame.density) || frame.density < 0.0f || !std::isfinite(frame.height_falloff) || frame.height_falloff < 0.0f) {
        return "the density and its height falloff must be finite and non-negative";
    }
    if (!finite(frame.albedo) || glm::any(glm::lessThan(frame.albedo, glm::vec3(0.0f))) || glm::any(glm::greaterThan(frame.albedo, glm::vec3(1.0f)))) {
        return "the albedo must be in [0, 1] per channel";
    }
    if (!(frame.anisotropy > -1.0f && frame.anisotropy < 1.0f)) {
        return "the anisotropy must be in (-1, 1)";
    }
    if (!finite(frame.sun_direction) || !finite(frame.sun_radiance) || glm::any(glm::lessThan(frame.sun_radiance, glm::vec3(0.0f)))) {
        return "the sun must be finite, with non-negative radiance";
    }
    if (!std::isfinite(frame.history_weight) || frame.history_weight < 0.0f || frame.history_weight >= 1.0f) {
        return "the history weight must be in [0, 1)";
    }
    if (frame.max_age < 1) {
        return "the max age must be at least 1";
    }
    if (frame.lights.size() > kFogMaxLights) {
        return "at most " + std::to_string(kFogMaxLights) + " local lights";
    }
    for (const FogLight& light : frame.lights) {
        if (!finite(light.position) || !finite(light.radiance) || !(light.range > 0.0f) || !std::isfinite(light.range)) {
            return "a local light needs a finite position and radiance and a positive range";
        }
        if (glm::any(glm::lessThan(light.radiance, glm::vec3(0.0f)))) {
            return "a local light's radiance must be non-negative";
        }
    }
    return std::nullopt;
}

float fog_slice_edge(const FogFrame& frame, std::uint32_t depth, float slice) noexcept {
    return frame.near_plane * std::pow(frame.far_plane / frame.near_plane, slice / static_cast<float>(depth));
}

float fog_density(const FogFrame& frame, float height) noexcept {
    return frame.density * std::exp(-frame.height_falloff * height);
}

float fog_phase(float cosine, float anisotropy) noexcept {
    const float g2 = anisotropy * anisotropy;
    const float denominator = std::max(1.0f + g2 - 2.0f * anisotropy * cosine, 1.0e-4f);
    return (1.0f - g2) / (4.0f * kPi * denominator * std::sqrt(denominator));
}

glm::vec3 fog_froxel_center(const FogFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t x, std::uint32_t y,
                            std::uint32_t z) noexcept {
    const float view_depth = fog_slice_edge(frame, depth, static_cast<float>(z) + 0.5f);
    const float ndc_x = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 1.0f;
    const float ndc_y = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
    return frame.eye + view_depth * (frame.forward + ndc_x * frame.right + ndc_y * frame.up);
}

std::vector<std::uint8_t> pack_fog_parameters(const FogFrame& frame, std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                              bool history_valid) {
    std::vector<std::uint8_t> bytes(kFogParameterBytes, 0);
    put(bytes, kParamEye, glm::vec4(frame.eye, 0.0f));
    put(bytes, kParamForward, glm::vec4(frame.forward, 0.0f));
    put(bytes, kParamRight, glm::vec4(frame.right, 0.0f));
    put(bytes, kParamUp, glm::vec4(frame.up, 0.0f));
    put(bytes, kParamPreviousEye, glm::vec4(frame.previous_eye, 0.0f));
    put(bytes, kParamPreviousForward, glm::vec4(frame.previous_forward, 0.0f));
    put(bytes, kParamPreviousRight, glm::vec4(frame.previous_right, 0.0f));
    put(bytes, kParamPreviousUp, glm::vec4(frame.previous_up, 0.0f));
    put(bytes, kParamSunDirection, glm::vec4(frame.sun_direction, 0.0f));
    put(bytes, kParamSunRadiance, glm::vec4(frame.sun_radiance, 0.0f));
    put(bytes, kParamAlbedo, frame.albedo);
    put(bytes, kParamDensity, frame.density);
    put(bytes, kParamHeightFalloff, frame.height_falloff);
    put(bytes, kParamAnisotropy, frame.anisotropy);
    put(bytes, kParamNear, frame.near_plane);
    put(bytes, kParamFar, frame.far_plane);
    put(bytes, kParamWidth, width);
    put(bytes, kParamHeight, height);
    put(bytes, kParamDepth, depth);
    put(bytes, kParamFlags, (frame.reset ? kFlagReset : 0u) | (history_valid ? kFlagHistoryValid : 0u) | (frame.shadows ? kFlagShadows : 0u));
    put(bytes, kParamHistoryWeight, frame.history_weight);
    put(bytes, kParamMaxAge, frame.max_age);
    put(bytes, kParamLightCount, static_cast<std::uint32_t>(frame.lights.size()));
    put(bytes, kParamCameraNear, frame.camera_near);
    put(bytes, kParamCameraFar, frame.camera_far);
    for (std::size_t i = 0; i < frame.lights.size(); ++i) {
        const std::uint32_t base = kParamLights + static_cast<std::uint32_t>(i) * kLightStride;
        put(bytes, base, glm::vec4(frame.lights[i].position, 0.0f));
        put(bytes, base + kLightRange, frame.lights[i].range);
        put(bytes, base + kLightRadiance, glm::vec4(frame.lights[i].radiance, 0.0f));
        put(bytes, base + kLightShadowed, static_cast<std::uint32_t>(frame.lights[i].shadowed ? 1 : 0));
    }
    return bytes;
}

FogVolume inject_reference(const FogFrame& frame, const FogVolume& previous, std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                           const FogVisibility& visibility) {
    FogVolume out;
    out.width = width;
    out.height = height;
    out.depth = depth;
    out.cells.assign(static_cast<std::size_t>(width) * height * depth, glm::vec4(0.0f));
    const bool use_history = !frame.reset && previous.width == width && previous.height == height && previous.depth == depth &&
                             previous.cells.size() == out.cells.size();

    for (std::uint32_t z = 0; z < depth; ++z) {
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const glm::vec3 point = fog_froxel_center(frame, width, height, depth, x, y, z);
                const glm::vec3 ray = glm::normalize(point - frame.eye);

                // Lighting: the sun, then each local light, each scaled by its phase and visibility.
                glm::vec3 lighting(0.0f);
                if (glm::any(glm::greaterThan(frame.sun_radiance, glm::vec3(0.0f)))) {
                    const float visible = frame.shadows ? visibility(point, point + frame.sun_direction * kFogSunDistance) : 1.0f;
                    lighting += frame.sun_radiance * fog_phase(glm::dot(frame.sun_direction, ray), frame.anisotropy) * visible;
                }
                for (const FogLight& light : frame.lights) {
                    const glm::vec3 to_light = light.position - point;
                    const float distance = glm::length(to_light);
                    if (!(distance > 1.0e-4f)) {
                        continue;
                    }
                    const glm::vec3 direction = to_light / distance;
                    const float attenuation = range_window(distance, light.range) / (1.0f + distance * distance);
                    const float visible = (light.shadowed && frame.shadows) ? visibility(point, light.position) : 1.0f;
                    lighting += light.radiance * attenuation * fog_phase(glm::dot(direction, ray), frame.anisotropy) * visible;
                }
                const glm::vec3 scatter = fog_density(frame, point.y) * frame.albedo * lighting;

                // Temporal stabilisation: the previous value at the reprojected centre, blended with the weight that never falls below
                // 1 / (age + 1).
                glm::vec4 cell(scatter, 1.0f);
                std::uint32_t px = 0;
                std::uint32_t py = 0;
                std::uint32_t pz = 0;
                if (use_history && previous_froxel(frame, width, height, depth, point, px, py, pz)) {
                    const glm::vec4 prior = previous.cells[previous.index(px, py, pz)];
                    if (finite(prior)) {
                        const float age = std::min(prior.a + 1.0f, static_cast<float>(frame.max_age));
                        const float alpha = std::max(1.0f - frame.history_weight, 1.0f / (age + 1.0f));
                        cell = glm::vec4(glm::mix(glm::vec3(prior), scatter, alpha), age);
                    }
                }
                out.cells[out.index(x, y, z)] = cell;
            }
        }
    }
    return out;
}

FogVolume integrate_reference(const FogFrame& frame, const FogVolume& injected) {
    FogVolume out = injected;
    for (std::uint32_t y = 0; y < injected.height; ++y) {
        for (std::uint32_t x = 0; x < injected.width; ++x) {
            glm::vec3 radiance(0.0f);
            float transmittance = 1.0f;
            for (std::uint32_t z = 0; z < injected.depth; ++z) {
                const float near_edge = fog_slice_edge(frame, injected.depth, static_cast<float>(z));
                const float far_edge = fog_slice_edge(frame, injected.depth, static_cast<float>(z) + 1.0f);
                const float length = far_edge - near_edge;
                const glm::vec3 point = fog_froxel_center(frame, injected.width, injected.height, injected.depth, x, y, z);
                const float extinction = fog_density(frame, point.y);
                const glm::vec3 scatter = glm::vec3(injected.cells[injected.index(x, y, z)]);
                // The in-scatter of a slice whose scattering and extinction are constant, weighted by the transmittance at its start.
                const glm::vec3 slice = extinction > 1.0e-8f ? scatter * (1.0f - std::exp(-extinction * length)) / extinction : scatter * length;
                radiance += transmittance * slice;
                transmittance *= std::exp(-extinction * length);
                out.cells[out.index(x, y, z)] = glm::vec4(radiance, transmittance);
            }
        }
    }
    return out;
}

glm::vec3 apply_fog(const glm::vec3& color, const FogVolume& integrated, const FogFrame& frame, std::uint32_t x, std::uint32_t y,
                    float view_depth) noexcept {
    const float slice = static_cast<float>(integrated.depth) * std::log(view_depth / frame.near_plane) / std::log(frame.far_plane / frame.near_plane);
    const std::uint32_t z = static_cast<std::uint32_t>(std::clamp(slice, 0.0f, static_cast<float>(integrated.depth - 1)));
    const glm::vec4 sample = integrated.cells[integrated.index(x, y, z)];
    return color * sample.a + glm::vec3(sample);
}

float fog_scene_view_depth(float ndc_depth, float camera_near, float camera_far) noexcept {
    if (!(ndc_depth > 0.0f)) {
        return camera_far;
    }
    const float ndc = std::min(ndc_depth, 1.0f);
    return camera_near * camera_far / (camera_far - ndc * (camera_far - camera_near));
}

RhiResult<std::unique_ptr<VolumetricFog>> VolumetricFog::create(rhi::Device& device, std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                                                rhi::ShaderBinary inject_shader, rhi::ShaderBinary integrate_shader,
                                                                rhi::ShaderBinary apply_shader) {
    if (width == 0 || height == 0 || depth == 0) {
        return rhi::fail<std::unique_ptr<VolumetricFog>>(RhiErrorCode::OutOfRange, "the fog grid needs a non-zero size");
    }
    if (!device.capabilities().enabled(rhi::Feature::RayTracing)) {
        return rhi::fail<std::unique_ptr<VolumetricFog>>(RhiErrorCode::FeatureDisabled, "volumetric fog needs the RayTracing feature for its shadows");
    }
    if (inject_shader.bytecode.empty() || integrate_shader.bytecode.empty() || apply_shader.bytecode.empty()) {
        return rhi::fail<std::unique_ptr<VolumetricFog>>(RhiErrorCode::InvalidState, "volumetric fog needs its inject, integrate, and apply shaders");
    }

    // Inject: t0 parameters, t1 the scene, t2 the previous grid, and u3 the next grid. Integrate: t0 parameters, t4 the grid, and u5 the output.
    // The registers differ from inject's so that both entries can live in shaders/volumetric_fog.hlsl.
    rhi::DescriptorSetLayoutDesc inject_desc;
    inject_desc.bindings = {
        rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{1, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{2, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{3, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute},
    };
    rhi::DescriptorSetLayoutDesc integrate_desc;
    integrate_desc.bindings = {
        rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{4, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{5, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute},
    };
    // Apply: t0 parameters, t6 the lit colour, t7 the scene's depth, t8 the integrated grid, and u10 the output. Its registers are apart from
    // the other two entries' so that all three can live in shaders/volumetric_fog.hlsl.
    rhi::DescriptorSetLayoutDesc apply_desc;
    apply_desc.bindings = {
        rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{6, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{7, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{8, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute},
        rhi::DescriptorBindingDesc{10, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute},
    };
    RhiResult<rhi::DescriptorSetLayoutHandle> inject_layout = device.create_descriptor_set_layout(inject_desc);
    if (!inject_layout.ok()) {
        return core::failure(inject_layout.error());
    }
    RhiResult<rhi::DescriptorSetLayoutHandle> integrate_layout = device.create_descriptor_set_layout(integrate_desc);
    if (!integrate_layout.ok()) {
        return core::failure(integrate_layout.error());
    }

    rhi::PipelineDesc inject_pipeline;
    inject_pipeline.kind = rhi::PipelineKind::Compute;
    inject_pipeline.shaders = {std::move(inject_shader)};
    inject_pipeline.set_layouts = {inject_layout.value()};
    inject_pipeline.debug_name = "volumetric fog inject";
    RhiResult<rhi::PipelineHandle> inject = device.create_pipeline(inject_pipeline);
    if (!inject.ok()) {
        return core::failure(inject.error());
    }
    rhi::PipelineDesc integrate_pipeline;
    integrate_pipeline.kind = rhi::PipelineKind::Compute;
    integrate_pipeline.shaders = {std::move(integrate_shader)};
    integrate_pipeline.set_layouts = {integrate_layout.value()};
    integrate_pipeline.debug_name = "volumetric fog integrate";
    RhiResult<rhi::PipelineHandle> integrate = device.create_pipeline(integrate_pipeline);
    if (!integrate.ok()) {
        return core::failure(integrate.error());
    }

    RhiResult<rhi::DescriptorSetLayoutHandle> apply_layout = device.create_descriptor_set_layout(apply_desc);
    if (!apply_layout.ok()) {
        return core::failure(apply_layout.error());
    }
    rhi::PipelineDesc apply_pipeline;
    apply_pipeline.kind = rhi::PipelineKind::Compute;
    apply_pipeline.shaders = {std::move(apply_shader)};
    apply_pipeline.set_layouts = {apply_layout.value()};
    apply_pipeline.debug_name = "volumetric fog apply";
    RhiResult<rhi::PipelineHandle> apply = device.create_pipeline(apply_pipeline);
    if (!apply.ok()) {
        return core::failure(apply.error());
    }

    std::unique_ptr<VolumetricFog> fog(new VolumetricFog(device, width, height, depth));
    fog->inject_layout_ = inject_layout.value();
    fog->integrate_layout_ = integrate_layout.value();
    fog->apply_layout_ = apply_layout.value();
    fog->inject_pipeline_ = inject.value();
    fog->integrate_pipeline_ = integrate.value();
    fog->apply_pipeline_ = apply.value();

    const std::uint64_t grid_bytes = static_cast<std::uint64_t>(width) * height * depth * kFogFroxelBytes;
    RhiResult<BufferHandle> parameters = device.create_buffer(detail::gpu_buffer_desc(kFogParameterBytes, kParameterUsage, "fog parameters"));
    if (!parameters.ok()) {
        return core::failure(parameters.error());
    }
    fog->parameters_ = parameters.value();
    for (int parity = 0; parity < 2; ++parity) {
        RhiResult<BufferHandle> grid = device.create_buffer(detail::gpu_buffer_desc(grid_bytes, kGridUsage, "fog grid"));
        if (!grid.ok()) {
            return core::failure(grid.error());
        }
        fog->grids_[parity] = grid.value();
    }
    // The integrated grid can be copied out, so a caller can read it back.
    RhiResult<BufferHandle> integrated =
        device.create_buffer(detail::gpu_buffer_desc(grid_bytes, kGridUsage | BufferUsage::CopySource, "fog integrated"));
    if (!integrated.ok()) {
        return core::failure(integrated.error());
    }
    fog->integrated_ = integrated.value();

    // Set p of inject reads grid p and writes grid 1 - p. Set p of integrate reads grid p.
    for (int parity = 0; parity < 2; ++parity) {
        const int next = 1 - parity;
        RhiResult<rhi::DescriptorSetHandle> inject_set = device.create_descriptor_set(inject_layout.value());
        if (!inject_set.ok()) {
            return core::failure(inject_set.error());
        }
        fog->inject_sets_[parity] = inject_set.value();
        RhiResult<rhi::DescriptorSetHandle> integrate_set = device.create_descriptor_set(integrate_layout.value());
        if (!integrate_set.ok()) {
            return core::failure(integrate_set.error());
        }
        fog->integrate_sets_[parity] = integrate_set.value();

        if (const RhiStatus written = device.write_buffer(inject_set.value(), 0, 0, fog->parameters_); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_buffer(inject_set.value(), 2, 0, fog->grids_[parity]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_buffer(inject_set.value(), 3, 0, fog->grids_[next]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_buffer(integrate_set.value(), 0, 0, fog->parameters_); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_buffer(integrate_set.value(), 4, 0, fog->grids_[parity]); !written.ok()) {
            return core::failure(written.error());
        }
        if (const RhiStatus written = device.write_buffer(integrate_set.value(), 5, 0, fog->integrated_); !written.ok()) {
            return core::failure(written.error());
        }
    }
    RhiResult<rhi::DescriptorSetHandle> apply_set = device.create_descriptor_set(apply_layout.value());
    if (!apply_set.ok()) {
        return core::failure(apply_set.error());
    }
    fog->apply_set_ = apply_set.value();
    if (const RhiStatus written = device.write_buffer(fog->apply_set_, 0, 0, fog->parameters_); !written.ok()) {
        return core::failure(written.error());
    }
    if (const RhiStatus written = device.write_buffer(fog->apply_set_, 8, 0, fog->integrated_); !written.ok()) {
        return core::failure(written.error());
    }
    return core::success(std::move(fog));
}

VolumetricFog::VolumetricFog(rhi::Device& device, std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept
    : device_(device), width_(width), height_(height), depth_(depth) {}

RhiStatus VolumetricFog::set_scene(rhi::AccelerationStructureHandle top_level) {
    if (!top_level.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "volumetric fog needs a built top-level structure");
    }
    for (const rhi::DescriptorSetHandle set : inject_sets_) {
        if (const RhiStatus written = device_.write_acceleration_structure(set, 1, 0, top_level); !written.ok()) {
            return written;
        }
    }
    top_level_ = top_level;
    scene_set_ = true;
    return core::success();
}

RhiStatus VolumetricFog::update_frame(const FogFrame& frame) {
    if (const std::optional<std::string> problem = validate_fog(frame, width_, height_, depth_)) {
        return rhi::fail(RhiErrorCode::OutOfRange, "volumetric fog frame: " + *problem);
    }
    const std::vector<std::uint8_t> bytes = pack_fog_parameters(frame, width_, height_, depth_, history_valid_);
    const ResourceState before = parameters_uploaded_ ? ResourceState::ShaderRead : ResourceState::Common;
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, parameters_, kParameterUsage, kFogParameterBytes, bytes, before);
        !uploaded.ok()) {
        return uploaded;
    }
    parameters_uploaded_ = true;
    frame_set_ = true;
    return core::success();
}

RhiStatus VolumetricFog::inject(rhi::CommandList& list) {
    if (!scene_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "volumetric fog: set_scene before inject");
    }
    if (!frame_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "volumetric fog: update_frame before inject");
    }
    const std::uint32_t previous = parity_;
    const std::uint32_t next = 1 - parity_;
    if (grid_state_[previous] != ResourceState::ShaderRead) {
        if (const RhiStatus status = list.transition(grids_[previous], grid_state_[previous], ResourceState::ShaderRead); !status.ok()) {
            return status;
        }
        grid_state_[previous] = ResourceState::ShaderRead;
    }
    if (grid_state_[next] != ResourceState::UnorderedAccess) {
        if (const RhiStatus status = list.transition(grids_[next], grid_state_[next], ResourceState::UnorderedAccess); !status.ok()) {
            return status;
        }
        grid_state_[next] = ResourceState::UnorderedAccess;
    }
    if (const RhiStatus status = list.set_pipeline(inject_pipeline_); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_descriptor_set(0, inject_sets_[previous]); !status.ok()) {
        return status;
    }
    // Eight by eight columns and four slices per group, as the shader's thread group.
    if (const RhiStatus status = list.dispatch((width_ + 7) / 8, (height_ + 7) / 8, (depth_ + 3) / 4); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.transition(grids_[next], ResourceState::UnorderedAccess, ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    grid_state_[next] = ResourceState::ShaderRead;
    parity_ = next;
    history_valid_ = true;
    injected_ = true;
    return core::success();
}

RhiStatus VolumetricFog::integrate(rhi::CommandList& list) {
    if (!injected_) {
        return rhi::fail(RhiErrorCode::InvalidState, "volumetric fog: inject before integrate");
    }
    if (integrated_state_ != ResourceState::UnorderedAccess) {
        if (const RhiStatus status = list.transition(integrated_, integrated_state_, ResourceState::UnorderedAccess); !status.ok()) {
            return status;
        }
    }
    if (const RhiStatus status = list.set_pipeline(integrate_pipeline_); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_descriptor_set(0, integrate_sets_[parity_]); !status.ok()) {
        return status;
    }
    // One thread per column: each walks its slices front to back.
    if (const RhiStatus status = list.dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.transition(integrated_, ResourceState::UnorderedAccess, ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    integrated_state_ = ResourceState::ShaderRead;
    integrated_once_ = true;
    return core::success();
}

RhiStatus VolumetricFog::set_apply_targets(const FogApplyTargets& targets) {
    if (!targets.color.valid() || !targets.depth.valid() || !targets.output.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "volumetric fog apply needs its colour, depth, and output targets");
    }
    if (targets.width == 0 || targets.height == 0) {
        return rhi::fail(RhiErrorCode::OutOfRange, "volumetric fog apply needs a non-zero target size");
    }
    if (const RhiStatus written = device_.write_texture(apply_set_, 6, 0, targets.color); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_texture(apply_set_, 7, 0, targets.depth); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_texture(apply_set_, 10, 0, targets.output); !written.ok()) {
        return written;
    }
    apply_width_ = targets.width;
    apply_height_ = targets.height;
    apply_bound_ = true;
    return core::success();
}

RhiStatus VolumetricFog::apply(rhi::CommandList& list) {
    if (!integrated_once_) {
        return rhi::fail(RhiErrorCode::InvalidState, "volumetric fog: integrate before apply");
    }
    if (!apply_bound_) {
        return rhi::fail(RhiErrorCode::InvalidState, "volumetric fog: set_apply_targets before apply");
    }
    if (const RhiStatus status = list.set_pipeline(apply_pipeline_); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_descriptor_set(0, apply_set_); !status.ok()) {
        return status;
    }
    // Eight by eight pixels per group, as the shader's thread group.
    return list.dispatch((apply_width_ + 7) / 8, (apply_height_ + 7) / 8, 1);
}

RhiStatus VolumetricFog::release() {
    RhiStatus first = core::success();
    const auto keep = [&first](RhiStatus status) {
        if (!status.ok() && first.ok()) {
            first = std::move(status);
        }
    };
    for (int parity = 0; parity < 2; ++parity) {
        keep(device_.destroy(inject_sets_[parity]));
        keep(device_.destroy(integrate_sets_[parity]));
        keep(device_.destroy(grids_[parity]));
    }
    keep(device_.destroy(inject_layout_));
    keep(device_.destroy(integrate_layout_));
    keep(device_.destroy(apply_layout_));
    keep(device_.destroy(apply_set_));
    keep(device_.destroy(inject_pipeline_));
    keep(device_.destroy(integrate_pipeline_));
    keep(device_.destroy(apply_pipeline_));
    keep(device_.destroy(parameters_));
    keep(device_.destroy(integrated_));
    scene_set_ = false;
    frame_set_ = false;
    injected_ = false;
    integrated_once_ = false;
    apply_bound_ = false;
    history_valid_ = false;
    return first;
}

}  // namespace engine::render
