#include "engine/render/particles.hpp"

#include "engine/core/coordinates.hpp"
#include "engine/core/result.hpp"
#include "gpu_upload.hpp"

#include <algorithm>
#include <array>
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
using rhi::ShaderStage;

// Byte offsets of the parameter block (shaders/particles.hlsl).
constexpr std::uint32_t kParamEmitter = 0;
constexpr std::uint32_t kParamDirection = 16;
constexpr std::uint32_t kParamSpeedLife = 32;
constexpr std::uint32_t kParamGravity = 48;
constexpr std::uint32_t kParamBoundsMin = 64;
constexpr std::uint32_t kParamBoundsMax = 80;
constexpr std::uint32_t kParamColorTimes = 96;
constexpr std::uint32_t kParamColors = 128;
constexpr std::uint32_t kParamSizeTimes = 256;
constexpr std::uint32_t kParamSizes = 288;
constexpr std::uint32_t kParamCounts = 416;
constexpr std::uint32_t kParamSim = 432;
constexpr std::uint32_t kParamTiming = 448;
constexpr std::uint32_t kParamFlags = 464;
constexpr std::uint32_t kParamCameraRight = 480;
constexpr std::uint32_t kParamCameraUp = 496;
constexpr std::uint32_t kParamViewProjection = 512;

constexpr std::uint32_t kCurveStride = 16;
constexpr std::uint32_t kCounterBytes = 16;
constexpr std::uint32_t kIndirectBytes = 20;
constexpr std::uint32_t kDrawIndexBytes = 4;
constexpr std::uint64_t kQuadIndexBytes = 6 * sizeof(std::uint32_t);
constexpr float kPi = 3.14159265358979323846f;

constexpr BufferUsage kParameterUsage = BufferUsage::ShaderResource | BufferUsage::CopyDestination;
constexpr BufferUsage kPoolUsage = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource | BufferUsage::CopySource;
constexpr BufferUsage kDrawUsage = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource;
constexpr BufferUsage kCounterUsage = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource | BufferUsage::CopySource |
                                      BufferUsage::CopyDestination;
constexpr BufferUsage kIndirectUsage = BufferUsage::UnorderedAccess | BufferUsage::IndirectArgument;
constexpr BufferUsage kQuadUsage = BufferUsage::Index | BufferUsage::CopyDestination;
constexpr BufferUsage kDummyVertexUsage = BufferUsage::Vertex | BufferUsage::CopyDestination;
constexpr BufferUsage kZeroUsage = BufferUsage::CopySource | BufferUsage::CpuWrite;

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void put_vec4(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const glm::vec3& value, float w) {
    put(bytes, offset, value.x);
    put(bytes, offset + 4, value.y);
    put(bytes, offset + 8, value.z);
    put(bytes, offset + 12, w);
}

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

rhi::RhiError error_of(const RhiStatus& status) {
    return status.error();
}

}  // namespace

std::optional<std::string> validate_curve(const ParticleCurve& curve) {
    if (curve.keys.empty() || curve.keys.size() > kParticleCurveKeys) {
        return "a curve has one to " + std::to_string(kParticleCurveKeys) + " keys";
    }
    for (std::size_t i = 0; i < curve.keys.size(); ++i) {
        const ParticleKey& key = curve.keys[i];
        if (!std::isfinite(key.time) || key.time < 0.0f || key.time > 1.0f) {
            return "a curve key time lies outside [0, 1]";
        }
        if (!std::isfinite(key.value.x) || !std::isfinite(key.value.y) || !std::isfinite(key.value.z) || !std::isfinite(key.value.w)) {
            return "a curve key value is not finite";
        }
        if (i > 0 && !(key.time > curve.keys[i - 1].time)) {
            return "curve key times must strictly increase";
        }
    }
    return std::nullopt;
}

glm::vec4 evaluate_curve(const ParticleCurve& curve, float t) noexcept {
    const std::vector<ParticleKey>& keys = curve.keys;
    if (keys.empty()) {
        return glm::vec4(1.0f);
    }
    if (t <= keys.front().time) {
        return keys.front().value;
    }
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        if (t <= keys[i + 1].time) {
            const float span = std::max(keys[i + 1].time - keys[i].time, 1e-6f);
            const float w = std::clamp((t - keys[i].time) / span, 0.0f, 1.0f);
            return glm::mix(keys[i].value, keys[i + 1].value, w);
        }
    }
    return keys.back().value;
}

std::optional<std::string> validate_emitter(const EmitterDesc& desc) {
    if (desc.capacity == 0) {
        return "an emitter needs a capacity of at least one particle";
    }
    if (!finite(desc.position) || !finite(desc.direction) || !finite(desc.gravity) || !finite(desc.bounds_min) || !finite(desc.bounds_max)) {
        return "an emitter vector is not finite";
    }
    if (glm::length(desc.direction) <= 0.0f) {
        return "an emitter needs a non-zero direction";
    }
    if (!std::isfinite(desc.spawn_radius) || desc.spawn_radius < 0.0f) {
        return "the spawn radius must be finite and not negative";
    }
    if (!std::isfinite(desc.spread) || desc.spread < 0.0f || desc.spread > kPi) {
        return "the spread must lie in [0, pi]";
    }
    if (!std::isfinite(desc.speed_min) || !std::isfinite(desc.speed_max) || desc.speed_min < 0.0f || desc.speed_max < desc.speed_min) {
        return "the speed range must be non-negative and in order";
    }
    if (!std::isfinite(desc.lifetime_min) || !std::isfinite(desc.lifetime_max) || desc.lifetime_min <= 0.0f || desc.lifetime_max < desc.lifetime_min) {
        return "the lifetime range must be positive and in order";
    }
    // A rate above a million per second would overflow the per-frame count long before the capacity clamp applies.
    if (!std::isfinite(desc.spawn_rate) || desc.spawn_rate < 0.0f || desc.spawn_rate > 1.0e6f) {
        return "the spawn rate must lie in [0, 1e6] particles per second";
    }
    if (desc.bounds_min.x > desc.bounds_max.x || desc.bounds_min.y > desc.bounds_max.y || desc.bounds_min.z > desc.bounds_max.z) {
        return "the bounds minimum must not exceed the maximum";
    }
    if (std::optional<std::string> problem = validate_curve(desc.color)) {
        return "colour curve: " + *problem;
    }
    if (std::optional<std::string> problem = validate_curve(desc.size)) {
        return "size curve: " + *problem;
    }
    return std::nullopt;
}

std::uint32_t advance_spawn(double& accumulator, double rate, double dt, std::uint32_t capacity, std::uint64_t& dropped) noexcept {
    if (!std::isfinite(rate) || !std::isfinite(dt) || rate < 0.0 || dt < 0.0) {
        return 0;
    }
    accumulator += rate * dt;
    const double whole = std::floor(accumulator);
    accumulator -= whole;
    if (whole > static_cast<double>(capacity)) {
        dropped += static_cast<std::uint64_t>(whole - static_cast<double>(capacity));
        return capacity;
    }
    return static_cast<std::uint32_t>(whole);
}

std::vector<std::uint8_t> pack_particle_parameters(const EmitterDesc& desc, const ParticleFrameState& state, const ParticleFrame& frame, bool vulkan) {
    std::vector<std::uint8_t> bytes(kParticleParameterBytes, 0);
    const glm::vec3 axis = glm::normalize(desc.direction);

    put_vec4(bytes, kParamEmitter, desc.position, desc.spawn_radius);
    put_vec4(bytes, kParamDirection, axis, std::cos(desc.spread));
    put(bytes, kParamSpeedLife, desc.speed_min);
    put(bytes, kParamSpeedLife + 4, desc.speed_max);
    put(bytes, kParamSpeedLife + 8, desc.lifetime_min);
    put(bytes, kParamSpeedLife + 12, desc.lifetime_max);
    put_vec4(bytes, kParamGravity, desc.gravity, 0.0f);
    put_vec4(bytes, kParamBoundsMin, desc.bounds_min, 0.0f);
    put_vec4(bytes, kParamBoundsMax, desc.bounds_max, 0.0f);

    for (std::size_t i = 0; i < desc.color.keys.size(); ++i) {
        put(bytes, kParamColorTimes + 4 * static_cast<std::uint32_t>(i), desc.color.keys[i].time);
        put(bytes, kParamColors + kCurveStride * static_cast<std::uint32_t>(i), desc.color.keys[i].value);
    }
    for (std::size_t i = 0; i < desc.size.keys.size(); ++i) {
        put(bytes, kParamSizeTimes + 4 * static_cast<std::uint32_t>(i), desc.size.keys[i].time);
        put(bytes, kParamSizes + kCurveStride * static_cast<std::uint32_t>(i), desc.size.keys[i].value);
    }
    put(bytes, kParamCounts, static_cast<std::uint32_t>(desc.color.keys.size()));
    put(bytes, kParamCounts + 4, static_cast<std::uint32_t>(desc.size.keys.size()));
    put(bytes, kParamCounts + 8, static_cast<std::uint32_t>(desc.blend == ParticleBlend::Additive ? 1u : 0u));

    put(bytes, kParamSim, desc.capacity);
    put(bytes, kParamSim + 4, state.spawn_count);
    put(bytes, kParamSim + 8, state.ring_head);
    put(bytes, kParamSim + 12, state.serial_base);
    put(bytes, kParamTiming, frame.dt);
    put(bytes, kParamFlags, static_cast<std::uint32_t>(state.kill_all ? 1u : 0u));
    put(bytes, kParamFlags + 4, desc.seed);

    put_vec4(bytes, kParamCameraRight, frame.camera_right, 0.0f);
    put_vec4(bytes, kParamCameraUp, frame.camera_up, 0.0f);
    const glm::mat4 clip = vulkan ? core::coords::to_vulkan_clip(frame.view_projection) : frame.view_projection;
    std::memcpy(bytes.data() + kParamViewProjection, &clip[0][0], sizeof(glm::mat4));
    return bytes;
}

// One emitter's GPU objects and its CPU spawn state. The pool, the draw list, the live counter, and the indirect record are its state
// machine: each records the state the last command left it in, so the next transition names it.
struct ParticleSystem::Emitter {
    EmitterDesc desc;
    BufferHandle parameters;
    BufferHandle pool;
    BufferHandle draw_list;
    BufferHandle counter;
    BufferHandle indirect;
    rhi::DescriptorSetHandle simulate_set;
    rhi::DescriptorSetHandle args_set;
    rhi::DescriptorSetHandle render_set;
    ResourceState pool_state = ResourceState::Common;
    ResourceState draw_list_state = ResourceState::Common;
    ResourceState counter_state = ResourceState::Common;
    ResourceState indirect_state = ResourceState::Common;
    double accumulator = 0.0;
    std::uint32_t head = 0;
    std::uint32_t serial = 0;
    std::uint64_t spawned = 0;
    std::uint64_t dropped = 0;
    bool paused = false;
    bool kill_pending = true;  // the pool holds no defined data until the first simulate kills it
    bool uploaded = false;     // the parameter buffer sits in ShaderRead after an upload
    bool prepared = false;     // prepare() advanced this emitter; simulate() records it
    bool simulated = false;    // simulate() has run at least once, so the indirect record is written
};

ParticleSystem::ParticleSystem(rhi::Device& device, ParticleBudget budget) noexcept : device_(device), budget_(budget) {}

ParticleSystem::~ParticleSystem() = default;

RhiResult<std::unique_ptr<ParticleSystem>> ParticleSystem::create(rhi::Device& device, rhi::ShaderBinary vertex_shader, rhi::ShaderBinary pixel_shader,
                                                                 rhi::ShaderBinary simulate_shader, rhi::ShaderBinary args_shader, ParticleBudget budget) {
    using Result = RhiResult<std::unique_ptr<ParticleSystem>>;
    if (budget.max_particles == 0 || budget.max_emitters == 0) {
        return rhi::fail<std::unique_ptr<ParticleSystem>>(RhiErrorCode::InvalidDescriptor, "particles: the budget needs a non-zero particle and emitter count");
    }
    if (vertex_shader.bytecode.empty() || pixel_shader.bytecode.empty() || simulate_shader.bytecode.empty() || args_shader.bytecode.empty()) {
        return rhi::fail<std::unique_ptr<ParticleSystem>>(RhiErrorCode::InvalidState, "particles: needs its vertex, pixel, simulate, and args shaders");
    }
    vertex_shader.stage = ShaderStage::Vertex;
    pixel_shader.stage = ShaderStage::Pixel;
    simulate_shader.stage = ShaderStage::Compute;
    args_shader.stage = ShaderStage::Compute;

    std::unique_ptr<ParticleSystem> system(new ParticleSystem(device, budget));
    // Every handle the system takes is recorded on it, so a failure part-way releases what was made.
    const auto abandon = [&system](rhi::RhiError error) -> Result {
        static_cast<void>(system->release());
        return core::failure(std::move(error));
    };

    // Simulate: t0 Params, u1 pool, u2 draw list, u3 live counter. Args: t4 live counter, u5 indirect record. Render: t0 Params, t1 pool, t2 draw
    // list, t3 the billboard texture. The register numbers are distinct across the file's globals (particles.hlsl).
    rhi::DescriptorSetLayoutDesc simulate_desc;
    simulate_desc.bindings = {
        rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, ShaderStage::Compute},
        rhi::DescriptorBindingDesc{1, rhi::DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
        rhi::DescriptorBindingDesc{2, rhi::DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
        rhi::DescriptorBindingDesc{3, rhi::DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
    };
    rhi::DescriptorSetLayoutDesc args_desc;
    args_desc.bindings = {
        rhi::DescriptorBindingDesc{4, rhi::DescriptorType::ShaderResource, 1, ShaderStage::Compute},
        rhi::DescriptorBindingDesc{5, rhi::DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
    };
    const ShaderStage draw_stages = ShaderStage::Vertex | ShaderStage::Pixel;
    rhi::DescriptorSetLayoutDesc render_desc;
    render_desc.bindings = {
        rhi::DescriptorBindingDesc{0, rhi::DescriptorType::ShaderResource, 1, draw_stages},
        rhi::DescriptorBindingDesc{1, rhi::DescriptorType::ShaderResource, 1, ShaderStage::Vertex},
        rhi::DescriptorBindingDesc{2, rhi::DescriptorType::ShaderResource, 1, ShaderStage::Vertex},
        rhi::DescriptorBindingDesc{3, rhi::DescriptorType::ShaderResource, 1, ShaderStage::Pixel},
    };

    RhiResult<rhi::DescriptorSetLayoutHandle> simulate_layout = device.create_descriptor_set_layout(simulate_desc);
    if (!simulate_layout.ok()) {
        return abandon(simulate_layout.error());
    }
    system->simulate_layout_ = simulate_layout.value();
    RhiResult<rhi::DescriptorSetLayoutHandle> args_layout = device.create_descriptor_set_layout(args_desc);
    if (!args_layout.ok()) {
        return abandon(args_layout.error());
    }
    system->args_layout_ = args_layout.value();
    RhiResult<rhi::DescriptorSetLayoutHandle> render_layout = device.create_descriptor_set_layout(render_desc);
    if (!render_layout.ok()) {
        return abandon(render_layout.error());
    }
    system->render_layout_ = render_layout.value();

    rhi::PipelineDesc simulate_pipeline;
    simulate_pipeline.kind = rhi::PipelineKind::Compute;
    simulate_pipeline.shaders = {std::move(simulate_shader)};
    simulate_pipeline.set_layouts = {system->simulate_layout_};
    simulate_pipeline.debug_name = "particles simulate";
    RhiResult<rhi::PipelineHandle> simulate = device.create_pipeline(simulate_pipeline);
    if (!simulate.ok()) {
        return abandon(simulate.error());
    }
    system->simulate_pipeline_ = simulate.value();

    rhi::PipelineDesc args_pipeline;
    args_pipeline.kind = rhi::PipelineKind::Compute;
    args_pipeline.shaders = {std::move(args_shader)};
    args_pipeline.set_layouts = {system->args_layout_};
    args_pipeline.debug_name = "particles indirect arguments";
    RhiResult<rhi::PipelineHandle> args = device.create_pipeline(args_pipeline);
    if (!args.ok()) {
        return abandon(args.error());
    }
    system->args_pipeline_ = args.value();

    // The billboards are drawn into HDR colour and tested against the scene's D32 depth, which they do not write.
    for (const ParticleBlend blend : {ParticleBlend::Alpha, ParticleBlend::Additive}) {
        rhi::PipelineDesc draw;
        draw.kind = rhi::PipelineKind::Graphics;
        draw.shaders = {vertex_shader, pixel_shader};
        draw.set_layouts = {system->render_layout_};
        draw.color_formats = {rhi::Format::R16G16B16A16Float};
        draw.depth_format = rhi::Format::D32Float;
        draw.depth_write = false;
        draw.cull_mode = rhi::CullMode::None;
        draw.alpha_blend = blend == ParticleBlend::Alpha;
        draw.additive_blend = blend == ParticleBlend::Additive;
        draw.debug_name = blend == ParticleBlend::Alpha ? "particles alpha" : "particles additive";
        RhiResult<rhi::PipelineHandle> pipeline = device.create_pipeline(draw);
        if (!pipeline.ok()) {
            return abandon(pipeline.error());
        }
        system->render_pipelines_[static_cast<std::size_t>(blend)] = pipeline.value();
    }

    // A unit quad: two triangles over the four corners that the vertex stage names by SV_VertexID.
    const std::array<std::uint32_t, 6> quad{0, 1, 2, 0, 2, 3};
    std::vector<std::uint8_t> quad_bytes(kQuadIndexBytes);
    std::memcpy(quad_bytes.data(), quad.data(), kQuadIndexBytes);
    RhiResult<BufferHandle> quad_buffer = device.create_buffer(detail::gpu_buffer_desc(kQuadIndexBytes, kQuadUsage, "particle quad indices"));
    if (!quad_buffer.ok()) {
        return abandon(quad_buffer.error());
    }
    system->quad_indices_ = quad_buffer.value();
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device, system->quad_indices_, kQuadUsage, kQuadIndexBytes, quad_bytes,
                                                                       ResourceState::Common, ResourceState::IndexBuffer);
        !uploaded.ok()) {
        return abandon(error_of(uploaded));
    }

    // The billboards read no vertex input, but the RHI's indexed draws need a vertex buffer bound, so a one-vertex buffer fills the slot.
    const std::vector<std::uint8_t> dummy_bytes(16, 0);
    RhiResult<BufferHandle> dummy = device.create_buffer(detail::gpu_buffer_desc(dummy_bytes.size(), kDummyVertexUsage, "particle vertex slot"));
    if (!dummy.ok()) {
        return abandon(dummy.error());
    }
    system->dummy_vertex_ = dummy.value();
    if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device, system->dummy_vertex_, kDummyVertexUsage, dummy_bytes.size(),
                                                                       dummy_bytes, ResourceState::Common, ResourceState::VertexBuffer);
        !uploaded.ok()) {
        return abandon(error_of(uploaded));
    }

    // The source of every counter reset: four zero bytes that CPU memory keeps zero. CpuWrite memory never leaves CopySource.
    RhiResult<BufferHandle> zero = device.create_buffer(detail::gpu_buffer_desc(4, kZeroUsage, "particle zero"));
    if (!zero.ok()) {
        return abandon(zero.error());
    }
    system->zero_ = zero.value();
    const std::array<std::uint8_t, 4> zeros{0, 0, 0, 0};
    if (const RhiStatus written = device.upload_buffer(system->zero_, 0, zeros.size(), zeros.data()); !written.ok()) {
        return abandon(error_of(written));
    }

    return core::success(std::move(system));
}

RhiResult<ParticleEmitterId> ParticleSystem::add_emitter(const EmitterDesc& desc) {
    if (released_) {
        return rhi::fail<ParticleEmitterId>(RhiErrorCode::InvalidState, "particles: the system is released");
    }
    if (std::optional<std::string> problem = validate_emitter(desc)) {
        return rhi::fail<ParticleEmitterId>(RhiErrorCode::InvalidDescriptor, "particles: " + *problem);
    }
    if (emitters_.size() >= budget_.max_emitters) {
        return rhi::fail<ParticleEmitterId>(RhiErrorCode::OutOfRange, "particles: the emitter count is at its budget");
    }
    if (static_cast<std::uint64_t>(allocated_) + desc.capacity > budget_.max_particles) {
        return rhi::fail<ParticleEmitterId>(RhiErrorCode::OutOfRange, "particles: the capacity does not fit the particle budget");
    }

    auto emitter = std::make_unique<Emitter>();
    emitter->desc = desc;
    const std::uint64_t capacity = desc.capacity;
    const auto build = [this, &emitter, capacity]() -> RhiStatus {
        RhiResult<BufferHandle> parameters = device_.create_buffer(detail::gpu_buffer_desc(kParticleParameterBytes, kParameterUsage, "particle parameters"));
        if (!parameters.ok()) {
            return core::failure(parameters.error());
        }
        emitter->parameters = parameters.value();
        RhiResult<BufferHandle> pool = device_.create_buffer(detail::gpu_buffer_desc(capacity * kParticleStrideBytes, kPoolUsage, "particle pool"));
        if (!pool.ok()) {
            return core::failure(pool.error());
        }
        emitter->pool = pool.value();
        RhiResult<BufferHandle> draw_list = device_.create_buffer(detail::gpu_buffer_desc(capacity * kDrawIndexBytes, kDrawUsage, "particle draw list"));
        if (!draw_list.ok()) {
            return core::failure(draw_list.error());
        }
        emitter->draw_list = draw_list.value();
        RhiResult<BufferHandle> counter = device_.create_buffer(detail::gpu_buffer_desc(kCounterBytes, kCounterUsage, "particle live counter"));
        if (!counter.ok()) {
            return core::failure(counter.error());
        }
        emitter->counter = counter.value();
        RhiResult<BufferHandle> indirect = device_.create_buffer(detail::gpu_buffer_desc(kIndirectBytes, kIndirectUsage, "particle indirect arguments"));
        if (!indirect.ok()) {
            return core::failure(indirect.error());
        }
        emitter->indirect = indirect.value();

        RhiResult<rhi::DescriptorSetHandle> simulate_set = device_.create_descriptor_set(simulate_layout_);
        if (!simulate_set.ok()) {
            return core::failure(simulate_set.error());
        }
        emitter->simulate_set = simulate_set.value();
        RhiResult<rhi::DescriptorSetHandle> args_set = device_.create_descriptor_set(args_layout_);
        if (!args_set.ok()) {
            return core::failure(args_set.error());
        }
        emitter->args_set = args_set.value();
        RhiResult<rhi::DescriptorSetHandle> render_set = device_.create_descriptor_set(render_layout_);
        if (!render_set.ok()) {
            return core::failure(render_set.error());
        }
        emitter->render_set = render_set.value();

        const rhi::DescriptorSetHandle sim = emitter->simulate_set;
        const rhi::DescriptorSetHandle args = emitter->args_set;
        const rhi::DescriptorSetHandle render = emitter->render_set;
        RhiStatus written = device_.write_buffer(sim, 0, 0, emitter->parameters);
        if (written.ok()) written = device_.write_buffer(sim, 1, 0, emitter->pool);
        if (written.ok()) written = device_.write_buffer(sim, 2, 0, emitter->draw_list);
        if (written.ok()) written = device_.write_buffer(sim, 3, 0, emitter->counter);
        if (written.ok()) written = device_.write_buffer(args, 4, 0, emitter->counter);
        if (written.ok()) written = device_.write_buffer(args, 5, 0, emitter->indirect);
        if (written.ok()) written = device_.write_buffer(render, 0, 0, emitter->parameters);
        if (written.ok()) written = device_.write_buffer(render, 1, 0, emitter->pool);
        if (written.ok()) written = device_.write_buffer(render, 2, 0, emitter->draw_list);
        if (written.ok() && texture_.valid()) written = device_.write_texture(render, 3, 0, texture_);
        return written;
    };
    if (const RhiStatus built = build(); !built.ok()) {
        static_cast<void>(destroy_emitter(*emitter));
        return core::failure(built.error());
    }

    const ParticleEmitterId id = next_id_++;
    allocated_ += desc.capacity;
    emitters_.emplace(id, std::move(emitter));
    order_.push_back(id);
    return core::success(id);
}

RhiStatus ParticleSystem::destroy_emitter(Emitter& emitter) {
    RhiStatus first = core::success();
    const auto keep = [&first](RhiStatus status) {
        if (!status.ok() && first.ok()) {
            first = std::move(status);
        }
    };
    if (emitter.render_set.valid()) keep(device_.destroy(emitter.render_set));
    if (emitter.args_set.valid()) keep(device_.destroy(emitter.args_set));
    if (emitter.simulate_set.valid()) keep(device_.destroy(emitter.simulate_set));
    if (emitter.indirect.valid()) keep(device_.destroy(emitter.indirect));
    if (emitter.counter.valid()) keep(device_.destroy(emitter.counter));
    if (emitter.draw_list.valid()) keep(device_.destroy(emitter.draw_list));
    if (emitter.pool.valid()) keep(device_.destroy(emitter.pool));
    if (emitter.parameters.valid()) keep(device_.destroy(emitter.parameters));
    return first;
}

RhiStatus ParticleSystem::remove_emitter(ParticleEmitterId id) {
    const auto found = emitters_.find(id);
    if (found == emitters_.end()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "particles: remove_emitter names no emitter");
    }
    allocated_ -= found->second->desc.capacity;
    const RhiStatus destroyed = destroy_emitter(*found->second);
    emitters_.erase(found);
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    return destroyed;
}

RhiStatus ParticleSystem::set_paused(ParticleEmitterId id, bool paused) {
    const auto found = emitters_.find(id);
    if (found == emitters_.end()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "particles: set_paused names no emitter");
    }
    found->second->paused = paused;
    return core::success();
}

RhiStatus ParticleSystem::reset(ParticleEmitterId id) {
    const auto found = emitters_.find(id);
    if (found == emitters_.end()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "particles: reset names no emitter");
    }
    Emitter& emitter = *found->second;
    emitter.accumulator = 0.0;
    emitter.head = 0;
    emitter.serial = 0;
    emitter.kill_pending = true;
    return core::success();
}

RhiStatus ParticleSystem::set_texture(rhi::TextureHandle texture) {
    if (!texture.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "particles: set_texture needs a valid texture");
    }
    texture_ = texture;
    RhiStatus first = core::success();
    for (const ParticleEmitterId id : order_) {
        const Emitter& emitter = *emitters_.at(id);
        RhiStatus written = device_.write_texture(emitter.render_set, 3, 0, texture);
        if (!written.ok() && first.ok()) {
            first = std::move(written);
        }
    }
    return first;
}

RhiStatus ParticleSystem::prepare(const ParticleFrame& frame) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "particles: the system is released");
    }
    if (!std::isfinite(frame.dt) || frame.dt < 0.0f) {
        return rhi::fail(RhiErrorCode::OutOfRange, "particles: the frame step must be finite and not negative");
    }
    const bool vulkan = device_.backend() == rhi::Backend::Vulkan;
    for (const ParticleEmitterId id : order_) {
        Emitter& emitter = *emitters_.at(id);
        emitter.prepared = false;
        if (emitter.paused) {
            continue;
        }
        // Advance copies, and commit them only once the upload has succeeded, so a failed frame leaves the spawn state as it was.
        ParticleFrameState state;
        state.kill_all = emitter.kill_pending;
        state.ring_head = emitter.head;
        state.serial_base = emitter.serial;
        double accumulator = emitter.accumulator;
        std::uint64_t dropped = emitter.dropped;
        state.spawn_count = advance_spawn(accumulator, emitter.desc.spawn_rate, frame.dt, emitter.desc.capacity, dropped);

        const std::vector<std::uint8_t> bytes = pack_particle_parameters(emitter.desc, state, frame, vulkan);
        const ResourceState before = emitter.uploaded ? ResourceState::ShaderRead : ResourceState::Common;
        if (const RhiStatus uploaded = detail::upload_buffer_through_graph(device_, emitter.parameters, kParameterUsage, kParticleParameterBytes, bytes,
                                                                          before);
            !uploaded.ok()) {
            return uploaded;
        }

        emitter.uploaded = true;
        emitter.accumulator = accumulator;
        emitter.dropped = dropped;
        emitter.head = (emitter.head + state.spawn_count) % emitter.desc.capacity;
        emitter.serial += state.spawn_count;
        emitter.spawned += state.spawn_count;
        emitter.kill_pending = false;
        emitter.prepared = true;
    }
    return core::success();
}

RhiStatus ParticleSystem::simulate(rhi::CommandList& list) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "particles: the system is released");
    }
    for (const ParticleEmitterId id : order_) {
        Emitter& emitter = *emitters_.at(id);
        if (!emitter.prepared) {
            continue;
        }
        const std::uint32_t capacity = emitter.desc.capacity;

        // Zero the live counter from the zero source, so the simulate pass counts this frame's live particles from nothing.
        if (const RhiStatus status = list.transition(emitter.counter, emitter.counter_state, ResourceState::CopyDestination); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.copy_buffer(zero_, 0, emitter.counter, 0, 4); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.transition(emitter.counter, ResourceState::CopyDestination, ResourceState::UnorderedAccess); !status.ok()) {
            return status;
        }
        emitter.counter_state = ResourceState::UnorderedAccess;

        // Spawn, integrate, and append the live slots to the draw list.
        if (const RhiStatus status = list.transition(emitter.pool, emitter.pool_state, ResourceState::UnorderedAccess); !status.ok()) {
            return status;
        }
        emitter.pool_state = ResourceState::UnorderedAccess;
        if (const RhiStatus status = list.transition(emitter.draw_list, emitter.draw_list_state, ResourceState::UnorderedAccess); !status.ok()) {
            return status;
        }
        emitter.draw_list_state = ResourceState::UnorderedAccess;
        if (const RhiStatus status = list.set_pipeline(simulate_pipeline_); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.set_descriptor_set(0, emitter.simulate_set); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.dispatch((capacity + kParticleThreadGroup - 1) / kParticleThreadGroup, 1, 1); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.transition(emitter.pool, ResourceState::UnorderedAccess, ResourceState::ShaderRead); !status.ok()) {
            return status;
        }
        emitter.pool_state = ResourceState::ShaderRead;
        if (const RhiStatus status = list.transition(emitter.draw_list, ResourceState::UnorderedAccess, ResourceState::ShaderRead); !status.ok()) {
            return status;
        }
        emitter.draw_list_state = ResourceState::ShaderRead;
        if (const RhiStatus status = list.transition(emitter.counter, ResourceState::UnorderedAccess, ResourceState::ShaderRead); !status.ok()) {
            return status;
        }
        emitter.counter_state = ResourceState::ShaderRead;

        // The indirect record takes the live count: six indices of one quad per live instance.
        if (const RhiStatus status = list.transition(emitter.indirect, emitter.indirect_state, ResourceState::UnorderedAccess); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.set_pipeline(args_pipeline_); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.set_descriptor_set(0, emitter.args_set); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.dispatch(1, 1, 1); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.transition(emitter.indirect, ResourceState::UnorderedAccess, ResourceState::IndirectArgument); !status.ok()) {
            return status;
        }
        emitter.indirect_state = ResourceState::IndirectArgument;
        emitter.simulated = true;
        emitter.prepared = false;
    }
    return core::success();
}

RhiStatus ParticleSystem::draw(rhi::CommandList& list, const ParticleTargets& targets) {
    if (released_) {
        return rhi::fail(RhiErrorCode::InvalidState, "particles: the system is released");
    }
    if (!texture_.valid()) {
        return rhi::fail(RhiErrorCode::InvalidState, "particles: set_texture before draw");
    }
    if (!targets.color.valid()) {
        return rhi::fail(RhiErrorCode::InvalidHandle, "particles: draw needs a colour target");
    }
    if (!targets.depth.has_value()) {
        return rhi::fail(RhiErrorCode::InvalidDescriptor, "particles: draw needs the scene depth target (the pipelines test against it)");
    }
    std::vector<const Emitter*> drawn;
    for (const ParticleEmitterId id : order_) {
        const Emitter& emitter = *emitters_.at(id);
        if (emitter.simulated) {
            drawn.push_back(&emitter);
        }
    }
    if (drawn.empty()) {
        return core::success();
    }

    rhi::RenderPassDesc pass;
    pass.color_targets = {targets.color};
    pass.depth_target = targets.depth;
    if (const RhiStatus status = list.begin_render_pass(pass); !status.ok()) {
        return status;
    }
    for (const Emitter* emitter : drawn) {
        if (const RhiStatus status = list.set_pipeline(render_pipelines_[static_cast<std::size_t>(emitter->desc.blend)]); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.set_descriptor_set(0, emitter->render_set); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.set_vertex_buffer(0, dummy_vertex_); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.set_index_buffer(quad_indices_, rhi::IndexFormat::Uint32); !status.ok()) {
            return status;
        }
        if (const RhiStatus status = list.draw_indexed_indirect(emitter->indirect, 0, 1); !status.ok()) {
            return status;
        }
    }
    return list.end_render_pass();
}

RhiResult<ParticleEmitterStats> ParticleSystem::stats(ParticleEmitterId id) const {
    const auto found = emitters_.find(id);
    if (found == emitters_.end()) {
        return rhi::fail<ParticleEmitterStats>(RhiErrorCode::InvalidHandle, "particles: stats names no emitter");
    }
    const Emitter& emitter = *found->second;
    ParticleEmitterStats stats;
    stats.capacity = emitter.desc.capacity;
    stats.spawned = emitter.spawned;
    stats.dropped = emitter.dropped;
    stats.paused = emitter.paused;
    return core::success(stats);
}

BufferHandle ParticleSystem::particle_pool(ParticleEmitterId id) const {
    const auto found = emitters_.find(id);
    return found == emitters_.end() ? BufferHandle{} : found->second->pool;
}

BufferHandle ParticleSystem::live_counter(ParticleEmitterId id) const {
    const auto found = emitters_.find(id);
    return found == emitters_.end() ? BufferHandle{} : found->second->counter;
}

BufferHandle ParticleSystem::indirect_arguments(ParticleEmitterId id) const {
    const auto found = emitters_.find(id);
    return found == emitters_.end() ? BufferHandle{} : found->second->indirect;
}

RhiStatus ParticleSystem::release() {
    RhiStatus first = core::success();
    const auto keep = [&first](RhiStatus status) {
        if (!status.ok() && first.ok()) {
            first = std::move(status);
        }
    };
    for (const ParticleEmitterId id : order_) {
        keep(destroy_emitter(*emitters_.at(id)));
    }
    emitters_.clear();
    order_.clear();
    allocated_ = 0;
    if (simulate_pipeline_.valid()) keep(device_.destroy(simulate_pipeline_));
    if (args_pipeline_.valid()) keep(device_.destroy(args_pipeline_));
    for (rhi::PipelineHandle& pipeline : render_pipelines_) {
        if (pipeline.valid()) keep(device_.destroy(pipeline));
        pipeline = rhi::PipelineHandle{};
    }
    if (simulate_layout_.valid()) keep(device_.destroy(simulate_layout_));
    if (args_layout_.valid()) keep(device_.destroy(args_layout_));
    if (render_layout_.valid()) keep(device_.destroy(render_layout_));
    if (quad_indices_.valid()) keep(device_.destroy(quad_indices_));
    if (dummy_vertex_.valid()) keep(device_.destroy(dummy_vertex_));
    if (zero_.valid()) keep(device_.destroy(zero_));
    simulate_pipeline_ = rhi::PipelineHandle{};
    args_pipeline_ = rhi::PipelineHandle{};
    simulate_layout_ = rhi::DescriptorSetLayoutHandle{};
    args_layout_ = rhi::DescriptorSetLayoutHandle{};
    render_layout_ = rhi::DescriptorSetLayoutHandle{};
    quad_indices_ = BufferHandle{};
    dummy_vertex_ = BufferHandle{};
    zero_ = BufferHandle{};
    texture_ = rhi::TextureHandle{};
    released_ = true;
    return first;
}

}  // namespace engine::render
