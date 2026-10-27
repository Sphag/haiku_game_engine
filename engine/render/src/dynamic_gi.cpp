#include "engine/render/dynamic_gi.hpp"

#include "engine/core/result.hpp"
#include "gpu_upload.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstring>
#include <utility>

namespace engine::render {

namespace {

using rhi::BufferHandle;
using rhi::BufferUsage;
using rhi::Feature;
using rhi::ResourceState;
using rhi::RhiErrorCode;
using rhi::RhiResult;
using rhi::RhiStatus;

// Byte offsets of the parameter block (shaders/dynamic_gi.hlsl, kParam*).
constexpr std::uint32_t kParamSpacing = 0;
constexpr std::uint32_t kParamRays = 4;
constexpr std::uint32_t kParamUpdateCount = 8;
constexpr std::uint32_t kParamShadowDistance = 12;
constexpr std::uint32_t kParamSunDirection = 16;
constexpr std::uint32_t kParamSunRadiance = 32;
constexpr std::uint32_t kParamSkyRadiance = 48;
constexpr std::uint32_t kParamBias = 60;
constexpr std::uint32_t kParamCameraWorld = 64;

// Byte offsets of the sampling block (shaders/scene.hlsl, kSample*).
constexpr std::uint32_t kSampleOrigin = 0;
constexpr std::uint32_t kSampleSpacing = 12;
constexpr std::uint32_t kSampleDims = 16;
constexpr std::uint32_t kSampleEnabled = 28;
constexpr std::uint32_t kSampleResidue = 32;

// Byte offsets of an update record (shaders/dynamic_gi.hlsl, kUpdate*).
constexpr std::uint32_t kUpdateSlot = 0;
constexpr std::uint32_t kUpdateWeight = 4;
constexpr std::uint32_t kUpdateCell = 8;

// The parameters and the plan are read by the shader, and filled by a copy. The history is read and written by the shader, and copied out
// for tests and debugging.
constexpr BufferUsage kTableUsage = BufferUsage::ShaderResource | BufferUsage::CopyDestination;
constexpr BufferUsage kHistoryUsage = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource | BufferUsage::CopyDestination | BufferUsage::CopySource;

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

rhi::BufferDesc buffer_desc(std::uint64_t size, BufferUsage usage, const char* name) {
    return detail::gpu_buffer_desc(size, usage, name);
}

bool finite(float value) {
    return std::isfinite(value);
}

}  // namespace

gi::DynamicGridDesc full_profile_grid() {
    gi::DynamicGridDesc desc;
    desc.dims = glm::uvec3(32u, 8u, 32u);
    desc.spacing = 4.0f;
    desc.rays_per_probe = 64;
    desc.update_budget = 512;  // 32 * 8 * 32 / 16
    desc.max_history = 64;
    desc.lighting_history = 4;
    return desc;
}

std::vector<std::uint8_t> pack_dynamic_parameters(const gi::DynamicGridDesc& grid, const gi::DynamicLighting& lighting,
                                                  const DynamicGiSettings& settings, std::uint32_t update_count, const glm::vec3& camera_world) {
    std::vector<std::uint8_t> bytes(kDynamicGiParameterBytes, 0);
    put(bytes, kParamSpacing, grid.spacing);
    put(bytes, kParamRays, grid.rays_per_probe);
    put(bytes, kParamUpdateCount, update_count);
    put(bytes, kParamShadowDistance, settings.shadow_distance);
    put(bytes, kParamSunDirection, lighting.sun_direction);
    put(bytes, kParamSunRadiance, lighting.sun_radiance);
    put(bytes, kParamSkyRadiance, lighting.sky_radiance);
    put(bytes, kParamBias, settings.bias);
    put(bytes, kParamCameraWorld, camera_world);
    return bytes;
}

std::vector<std::uint8_t> pack_dynamic_sampling(const gi::DynamicGrid& grid) {
    const gi::DynamicGridDesc& desc = grid.desc();
    const glm::ivec3 origin = grid.origin();
    // A window cell with local index l on an axis sits at slot coordinate (residue + l) mod dims, where residue is the origin's residue.
    // The shader needs only the residue beyond the window, so it can find the slot without a table.
    glm::uvec3 residue{0u};
    for (int axis = 0; axis < 3; ++axis) {
        const auto dim = static_cast<std::int32_t>(desc.dims[axis]);
        residue[axis] = static_cast<std::uint32_t>(((origin[axis] % dim) + dim) % dim);
    }
    const std::size_t slots = grid.probe_count();
    std::vector<std::uint8_t> bytes(kDynamicGiSamplingHeaderBytes + slots * sizeof(std::uint32_t), 0);
    put(bytes, kSampleOrigin, origin);
    put(bytes, kSampleSpacing, desc.spacing);
    put(bytes, kSampleDims, desc.dims);
    put(bytes, kSampleEnabled, std::uint32_t{1});
    put(bytes, kSampleResidue, residue);
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        put(bytes, static_cast<std::uint32_t>(kDynamicGiSamplingHeaderBytes + slot * sizeof(std::uint32_t)),
            std::uint32_t{grid.samples(slot) > 0 ? 1u : 0u});
    }
    return bytes;
}

std::vector<std::uint8_t> pack_dynamic_updates(const std::vector<gi::ProbeUpdate>& updates) {
    std::vector<std::uint8_t> bytes(updates.size() * kDynamicGiUpdateStride, 0);
    for (std::size_t i = 0; i < updates.size(); ++i) {
        const auto base = static_cast<std::uint32_t>(i * kDynamicGiUpdateStride);
        put(bytes, base + kUpdateSlot, updates[i].slot);
        put(bytes, base + kUpdateWeight, updates[i].weight);
        put(bytes, base + kUpdateCell, updates[i].cell);
    }
    return bytes;
}

RhiResult<std::unique_ptr<DynamicGi>> DynamicGi::create(rhi::Device& device, const gi::DynamicGridDesc& desc, rhi::ShaderBinary shader,
                                                        DynamicGiSettings settings) {
    if (!device.capabilities().enabled(Feature::RayTracing)) {
        return rhi::fail<std::unique_ptr<DynamicGi>>(RhiErrorCode::FeatureDisabled, "dynamic GI needs the RayTracing feature");
    }
    if (shader.bytecode.empty()) {
        return rhi::fail<std::unique_ptr<DynamicGi>>(RhiErrorCode::InvalidState, "dynamic GI needs the compute shader (CSDynamicGi)");
    }
    if (!finite(settings.shadow_distance) || settings.shadow_distance <= 0.0f || !finite(settings.bias) || settings.bias < 0.0f) {
        return rhi::fail<std::unique_ptr<DynamicGi>>(RhiErrorCode::OutOfRange, "dynamic GI: the shadow distance must be positive and the bias non-negative");
    }
    core::Result<gi::DynamicGrid, std::string> grid = gi::DynamicGrid::create(desc);
    if (!grid.ok()) {
        return rhi::fail<std::unique_ptr<DynamicGi>>(RhiErrorCode::InvalidDescriptor, "dynamic GI grid: " + grid.error());
    }
    const std::uint64_t slots = grid.value().probe_count();
    const std::uint64_t history_bytes = slots * kDynamicGiProbeStride;

    // t0 parameters, t1 the scene, t2 instances, t3 triangles, t4 the plan; u5 the history.
    rhi::DescriptorSetLayoutDesc layout_desc;
    for (std::uint32_t binding = 0; binding <= 4; ++binding) {
        layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{binding, rhi::DescriptorType::ShaderResource, 1, rhi::ShaderStage::Compute});
    }
    layout_desc.bindings.push_back(rhi::DescriptorBindingDesc{5, rhi::DescriptorType::UnorderedAccess, 1, rhi::ShaderStage::Compute});
    RhiResult<rhi::DescriptorSetLayoutHandle> layout = device.create_descriptor_set_layout(layout_desc);
    if (!layout.ok()) {
        return core::failure(layout.error());
    }

    rhi::PipelineDesc pipeline_desc;
    pipeline_desc.kind = rhi::PipelineKind::Compute;
    pipeline_desc.shaders = {std::move(shader)};
    pipeline_desc.set_layouts = {layout.value()};
    pipeline_desc.debug_name = "dynamic GI";
    RhiResult<rhi::PipelineHandle> pipeline = device.create_pipeline(pipeline_desc);
    if (!pipeline.ok()) {
        return core::failure(pipeline.error());
    }

    RhiResult<rhi::DescriptorSetHandle> set = device.create_descriptor_set(layout.value());
    if (!set.ok()) {
        return core::failure(set.error());
    }
    RhiResult<BufferHandle> parameters = device.create_buffer(buffer_desc(kDynamicGiParameterBytes, kTableUsage, "dynamic GI parameters"));
    if (!parameters.ok()) {
        return core::failure(parameters.error());
    }
    RhiResult<BufferHandle> updates =
        device.create_buffer(buffer_desc(static_cast<std::uint64_t>(desc.update_budget) * kDynamicGiUpdateStride, kTableUsage, "dynamic GI plan"));
    if (!updates.ok()) {
        return core::failure(updates.error());
    }
    RhiResult<BufferHandle> history = device.create_buffer(buffer_desc(history_bytes, kHistoryUsage, "dynamic GI history"));
    if (!history.ok()) {
        return core::failure(history.error());
    }
    const std::uint64_t sampling_bytes = kDynamicGiSamplingHeaderBytes + slots * sizeof(std::uint32_t);
    RhiResult<BufferHandle> sampling = device.create_buffer(buffer_desc(sampling_bytes, kTableUsage, "dynamic GI sampling"));
    if (!sampling.ok()) {
        return core::failure(sampling.error());
    }

    std::unique_ptr<DynamicGi> pass(new DynamicGi(device, std::move(grid).value(), settings));
    pass->layout_ = layout.value();
    pass->set_ = set.value();
    pass->pipeline_ = pipeline.value();
    pass->parameters_ = parameters.value();
    pass->updates_ = updates.value();
    pass->history_ = history.value();
    pass->sampling_ = sampling.value();

    // The history must hold zeros before the first blend: a weight of one replaces the value, but a NaN left in memory would survive the
    // lerp of any weight below one. The graph's upload leaves it in ShaderRead.
    const std::vector<std::uint8_t> zeros(history_bytes, 0);
    if (const RhiStatus cleared = detail::upload_buffer_through_graph(device, pass->history_, kHistoryUsage, history_bytes, zeros, ResourceState::Common);
        !cleared.ok()) {
        return core::failure(cleared.error());
    }
    pass->history_state_ = ResourceState::ShaderRead;

    // Until the first update the sampling block is disabled, so no probe is read from the zeroed history.
    const std::vector<std::uint8_t> disabled(kDynamicGiSamplingHeaderBytes, 0);
    if (const RhiStatus cleared = detail::upload_buffer_through_graph(device, pass->sampling_, kTableUsage, kDynamicGiSamplingHeaderBytes, disabled,
                                                                      ResourceState::Common);
        !cleared.ok()) {
        return core::failure(cleared.error());
    }
    pass->sampling_uploaded_ = true;  // the copy leaves it in ShaderRead, so the first update starts from there

    if (const RhiStatus written = device.write_buffer(pass->set_, 0, 0, pass->parameters_); !written.ok()) {
        return core::failure(written.error());
    }
    if (const RhiStatus written = device.write_buffer(pass->set_, 4, 0, pass->updates_); !written.ok()) {
        return core::failure(written.error());
    }
    if (const RhiStatus written = device.write_buffer(pass->set_, 5, 0, pass->history_); !written.ok()) {
        return core::failure(written.error());
    }
    return core::success(std::move(pass));
}

DynamicGi::DynamicGi(rhi::Device& device, gi::DynamicGrid grid, DynamicGiSettings settings) noexcept
    : device_(device), grid_(std::move(grid)), settings_(settings) {}

RhiStatus DynamicGi::set_scene(const RtScene& scene) {
    if (!scene.top_level.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "dynamic GI needs a built top-level structure");
    }
    if (const std::optional<std::string> problem = validate_scene_records(scene.instances, scene.triangles)) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "dynamic GI scene: " + *problem);
    }

    const std::vector<std::uint8_t> instance_bytes = pack_instance_table(scene.instances);
    const std::vector<std::uint8_t> triangle_bytes = pack_triangle_table(scene.triangles);
    RhiResult<BufferHandle> instances = device_.create_buffer(buffer_desc(instance_bytes.size(), kTableUsage, "dynamic GI instances"));
    if (!instances.ok()) {
        return core::failure(instances.error());
    }
    RhiResult<BufferHandle> triangles = device_.create_buffer(buffer_desc(triangle_bytes.size(), kTableUsage, "dynamic GI triangles"));
    if (!triangles.ok()) {
        return core::failure(triangles.error());
    }
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, instances.value(), kTableUsage, instance_bytes.size(), instance_bytes,
                                                                        ResourceState::Common);
        !uploaded.ok()) {
        return uploaded;
    }
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, triangles.value(), kTableUsage, triangle_bytes.size(), triangle_bytes,
                                                                        ResourceState::Common);
        !uploaded.ok()) {
        return uploaded;
    }

    // The previous tables are no longer referenced once the descriptors point at the new ones. The RHI keeps their storage until the GPU is done.
    if (instances_.valid()) {
        static_cast<void>(device_.destroy(instances_));
    }
    if (triangles_.valid()) {
        static_cast<void>(device_.destroy(triangles_));
    }
    instances_ = instances.value();
    triangles_ = triangles.value();

    if (const RhiStatus written = device_.write_acceleration_structure(set_, 1, 0, scene.top_level); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_buffer(set_, 2, 0, instances_); !written.ok()) {
        return written;
    }
    if (const RhiStatus written = device_.write_buffer(set_, 3, 0, triangles_); !written.ok()) {
        return written;
    }
    scene_set_ = true;
    return core::success();
}

RhiStatus DynamicGi::update_frame(const glm::vec3& camera_world) {
    if (!scene_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "dynamic GI: set_scene before update_frame");
    }
    plan_ = grid_.plan_updates();
    const auto count = static_cast<std::uint32_t>(plan_.size());

    const std::vector<std::uint8_t> parameters = pack_dynamic_parameters(grid_.desc(), grid_.lighting(), settings_, count, camera_world);
    const ResourceState parameters_before = parameters_uploaded_ ? ResourceState::ShaderRead : ResourceState::Common;
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, parameters_, kTableUsage, kDynamicGiParameterBytes, parameters, parameters_before);
        !uploaded.ok()) {
        return uploaded;
    }
    parameters_uploaded_ = true;

    const std::vector<std::uint8_t> updates = pack_dynamic_updates(plan_);
    const ResourceState updates_before = updates_uploaded_ ? ResourceState::ShaderRead : ResourceState::Common;
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, updates_, kTableUsage,
                                                                        static_cast<std::uint64_t>(grid_.desc().update_budget) * kDynamicGiUpdateStride,
                                                                        updates, updates_before);
        !uploaded.ok()) {
        return uploaded;
    }
    updates_uploaded_ = true;
    // The valid flags are the samples before this frame's trace, so a probe counts only once a trace was recorded for it.
    const std::vector<std::uint8_t> sampling = pack_dynamic_sampling(grid_);
    const ResourceState sampling_before = sampling_uploaded_ ? ResourceState::ShaderRead : ResourceState::Common;
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, sampling_, kTableUsage, sampling.size(), sampling, sampling_before);
        !uploaded.ok()) {
        return uploaded;
    }
    sampling_uploaded_ = true;
    planned_ = true;
    return core::success();
}

RhiStatus DynamicGi::record(rhi::CommandList& list) {
    if (!scene_set_) {
        return rhi::fail(RhiErrorCode::InvalidState, "dynamic GI: set_scene before record");
    }
    if (!planned_) {
        return rhi::fail(RhiErrorCode::InvalidState, "dynamic GI: update_frame before record");
    }
    if (const RhiStatus status = list.transition(history_, history_state_, ResourceState::UnorderedAccess); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_pipeline(pipeline_); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.set_descriptor_set(0, set_); !status.ok()) {
        return status;
    }
    const auto count = static_cast<std::uint32_t>(plan_.size());
    if (const RhiStatus status = list.dispatch((count + kDynamicGiGroupSize - 1) / kDynamicGiGroupSize, 1, 1); !status.ok()) {
        return status;
    }
    if (const RhiStatus status = list.transition(history_, ResourceState::UnorderedAccess, ResourceState::ShaderRead); !status.ok()) {
        return status;
    }
    history_state_ = ResourceState::ShaderRead;
    // The traces are recorded, so their samples count. A probe that moved since it was planned is skipped by commit().
    grid_.commit(plan_);
    planned_ = false;
    return core::success();
}

RhiStatus DynamicGi::release() {
    RhiStatus first = core::success();
    const auto keep = [&first](RhiStatus status) {
        if (!status.ok() && first.ok()) {
            first = std::move(status);
        }
    };
    keep(device_.destroy(set_));
    keep(device_.destroy(layout_));
    keep(device_.destroy(pipeline_));
    keep(device_.destroy(parameters_));
    keep(device_.destroy(updates_));
    keep(device_.destroy(history_));
    keep(device_.destroy(sampling_));
    if (instances_.valid()) {
        keep(device_.destroy(instances_));
    }
    if (triangles_.valid()) {
        keep(device_.destroy(triangles_));
    }
    scene_set_ = false;
    planned_ = false;
    return first;
}

}  // namespace engine::render
