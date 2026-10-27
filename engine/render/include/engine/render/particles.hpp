#pragma once

// GPU particles (US-051). Each emitter owns a ring of particle slots, a draw list, a live counter, and an indirect argument record. Per frame:
//   prepare()   on the CPU: advances the emitter's spawn accumulator, and uploads the emitter's parameters (the spawn range, the seed, the
//               camera, the curves). A paused emitter is skipped. A reset emitter kills every particle on its next simulate.
//   simulate()  records, per emitter, a compute pass that spawns this frame's particles into their ring slots, integrates the live ones
//               (velocity, gravity, lifetime, bounds), and appends each live slot to the draw list through an atomic counter; a second
//               one-thread pass writes the indirect arguments from that counter.
//   draw()      records one render pass that draws every simulated emitter as an indexed indirect draw of camera-facing quads. Each quad
//               samples the caller's texture, is tinted by the colour curve and scaled by the size curve, and is depth-tested against the
//               scene's depth (without writing it). The alpha mode blends by source alpha; the additive mode adds the source to the target.
//
// Spawn. An emitter spawns spawn_rate particles per second. The accumulator keeps the fraction, so the spawn count per frame is exact. A
// frame's count is clamped to the emitter's capacity, and the clamped part is counted as dropped. The ring then overwrites the oldest slots, so
// the live count is bounded by the capacity, which is the budget. Each spawn draws its random values from a hash of (seed, spawn serial), so
// one backend reproduces the same particles from the same inputs.
//
// Integration. A particle is alive while age < life. Velocity takes gravity each step, and position takes velocity (semi-implicit Euler).
// A particle that leaves the emitter's bounds box is killed. The curves are evaluated at age / life.
//
// Budget. The system holds at most ParticleBudget::max_particles slots over all emitters, and at most max_emitters emitters. Adding an
// emitter that exceeds either fails with OutOfRange and allocates nothing.
//
// Resources. An emitter holds five buffers and three descriptor sets. remove_emitter() destroys them; reset() kills every particle and
// restarts the spawn sequence without releasing anything. The RHI retires destroyed storage behind its fences.
//
// The pass uses no vertex buffers. Its index buffer is a unit quad, and all four vertices come from SV_VertexID. The HLSL is in
// shaders/particles.hlsl; the byte layout of the parameter block is documented there and in pack_particle_parameters below.

#include "engine/rhi/command_list.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::render {

// Keys per curve, and the sizes of the GPU records (see shaders/particles.hlsl).
inline constexpr std::uint32_t kParticleCurveKeys = 8;
inline constexpr std::uint32_t kParticleStrideBytes = 32;
inline constexpr std::uint32_t kParticleParameterBytes = 576;
inline constexpr std::uint32_t kParticleThreadGroup = 64;
// The default budget (the PRD's render_stress and all_features scenes need 100 000 and 50 000 live particles on the full profile).
inline constexpr std::uint32_t kParticleDefaultBudget = 131072;
inline constexpr std::uint32_t kParticleDefaultMaxEmitters = 64;

// One key of a curve. The value is RGBA for the colour curve; the size curve reads the x component.
struct ParticleKey {
    float time = 0.0f;  // age / life in [0, 1]
    glm::vec4 value{1.0f};
};

// A curve through keys sorted by strictly increasing time, linearly interpolated. The value holds the first key's value before it and the
// last key's value after it. A curve has one to kParticleCurveKeys keys.
struct ParticleCurve {
    std::vector<ParticleKey> keys{ParticleKey{}};
};

// Checks a curve: one to kParticleCurveKeys keys, finite values, and times in [0, 1] that strictly increase. Returns the first problem.
[[nodiscard]] std::optional<std::string> validate_curve(const ParticleCurve& curve);
// The curve at age fraction `t` (clamped to [0, 1]). The curve must be valid.
[[nodiscard]] glm::vec4 evaluate_curve(const ParticleCurve& curve, float t) noexcept;

// The blend of a particle's draw. Alpha: src * a + dst * (1 - a). Additive: src + dst, which brightens and does not darken.
enum class ParticleBlend : std::uint8_t {
    Alpha,
    Additive,
};

struct EmitterDesc {
    std::string name = "emitter";
    glm::vec3 position{0.0f};
    float spawn_radius = 0.0f;  // particles start uniformly in a box of half-extent spawn_radius around position (meters)
    glm::vec3 direction{0.0f, 1.0f, 0.0f};  // the cone's axis; non-zero, and normalised by the packer
    float spread = 0.0f;        // the cone's half-angle in radians, in [0, pi]
    float speed_min = 1.0f;     // m/s
    float speed_max = 1.0f;
    float lifetime_min = 1.0f;  // seconds; must be positive
    float lifetime_max = 1.0f;
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};  // m/s^2
    float spawn_rate = 10.0f;               // particles per second, not negative
    glm::vec3 bounds_min{-1.0e4f};          // particles outside [bounds_min, bounds_max] are killed
    glm::vec3 bounds_max{1.0e4f};
    ParticleCurve color;  // RGBA over the particle's life
    ParticleCurve size;   // billboard edge length in meters over the particle's life (x component)
    ParticleBlend blend = ParticleBlend::Alpha;
    std::uint32_t capacity = 1024;  // live-particle budget of this emitter; the ring holds this many slots
    std::uint32_t seed = 1;         // the spawn random sequence; same seed, same backend, same particles
};

// Checks an emitter: a positive capacity, finite values, a non-zero direction, a positive lifetime range in order, a spread in [0, pi], a
// non-negative spawn rate,
// a speed range in order, bounds in order, and valid curves. Returns the first problem.
[[nodiscard]] std::optional<std::string> validate_emitter(const EmitterDesc& desc);

// Advances a spawn accumulator by rate * dt and returns the whole particles it now owes, which are removed from it. The result is clamped to
// capacity; the clamped part is added to `dropped`. A negative or non-finite rate or step spawns nothing.
[[nodiscard]] std::uint32_t advance_spawn(double& accumulator, double rate, double dt, std::uint32_t capacity, std::uint64_t& dropped) noexcept;

// The per-frame camera and time for every emitter. view_projection is the engine's clip convention (the DX12 depth range, y up); the
// system converts it for Vulkan.
struct ParticleFrame {
    glm::mat4 view_projection{1.0f};
    glm::vec3 camera_right{1.0f, 0.0f, 0.0f};
    glm::vec3 camera_up{0.0f, 1.0f, 0.0f};
    float dt = 1.0f / 60.0f;  // seconds; not negative
};

// The state of one emitter that the parameter block carries for one frame.
struct ParticleFrameState {
    std::uint32_t spawn_count = 0;  // particles spawned this frame (already clamped to the capacity)
    std::uint32_t ring_head = 0;    // the first slot of this frame's spawns
    std::uint32_t serial_base = 0;  // the spawn serial of this frame's first particle
    bool kill_all = false;          // kill every particle this frame (a reset)
};

// The byte image of the parameter block (layout in shaders/particles.hlsl). The matrix is converted for `vulkan` as the scene renderer does.
[[nodiscard]] std::vector<std::uint8_t> pack_particle_parameters(const EmitterDesc& desc, const ParticleFrameState& state, const ParticleFrame& frame,
                                                                  bool vulkan);

// The targets of a particle draw: the HDR colour (R16G16B16A16Float, RenderTarget state) and the scene depth (D32Float, DepthWrite state; it is
// tested and not written). The caller declares the states.
struct ParticleTargets {
    rhi::TextureHandle color;
    std::optional<rhi::TextureHandle> depth;
};

// Identifies an emitter. Ids are never reused, so a removed emitter's id stays invalid.
using ParticleEmitterId = std::uint32_t;

// Counts for one emitter, for tests and the editor.
struct ParticleEmitterStats {
    std::uint32_t capacity = 0;
    std::uint64_t spawned = 0;  // particles spawned so far (after clamping)
    std::uint64_t dropped = 0;  // particles requested but clamped by the capacity
    bool paused = false;
};

struct ParticleBudget {
    std::uint32_t max_particles = kParticleDefaultBudget;   // total capacity over every emitter
    std::uint32_t max_emitters = kParticleDefaultMaxEmitters;
};

class ParticleSystem {
public:
    // Creates the pipelines and the shared resources. The shaders are the four entries of shaders/particles.hlsl. Fails with InvalidDescriptor
    // for a budget of zero, and with InvalidState for a missing shader.
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<ParticleSystem>> create(rhi::Device& device, rhi::ShaderBinary vertex_shader,
                                                                                rhi::ShaderBinary pixel_shader, rhi::ShaderBinary simulate_shader,
                                                                                rhi::ShaderBinary args_shader, ParticleBudget budget = {});
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;
    ~ParticleSystem();

    // Creates an emitter. Fails with InvalidDescriptor for an invalid description and with OutOfRange when the budget cannot hold it.
    [[nodiscard]] rhi::RhiResult<ParticleEmitterId> add_emitter(const EmitterDesc& desc);
    // Destroys the emitter's buffers and descriptor sets. Fails with InvalidState when the id is unknown.
    [[nodiscard]] rhi::RhiStatus remove_emitter(ParticleEmitterId id);

    // A paused emitter spawns nothing and integrates nothing, and it keeps drawing its last state. Pausing is free and can be undone.
    [[nodiscard]] rhi::RhiStatus set_paused(ParticleEmitterId id, bool paused);
    // Kills every particle and restarts the spawn sequence at the emitter's first serial. The change applies on the next prepare().
    [[nodiscard]] rhi::RhiStatus reset(ParticleEmitterId id);

    // The texture the billboards sample. It must be in ShaderRead when draw() records. Sets it on every emitter's render set.
    [[nodiscard]] rhi::RhiStatus set_texture(rhi::TextureHandle texture);

    // Advances every unpaused emitter's spawn state and uploads its parameters. Call once per frame, before simulate(). The upload executes
    // through the render graph, which waits for it.
    [[nodiscard]] rhi::RhiStatus prepare(const ParticleFrame& frame);
    // Records the simulation of every emitter prepared since the last simulate().
    [[nodiscard]] rhi::RhiStatus simulate(rhi::CommandList& list);
    // Records the draw of every emitter that has been simulated at least once. Needs set_texture() and the targets in their states.
    [[nodiscard]] rhi::RhiStatus draw(rhi::CommandList& list, const ParticleTargets& targets);

    [[nodiscard]] rhi::RhiResult<ParticleEmitterStats> stats(ParticleEmitterId id) const;
    // The GPU buffers of an emitter, for readback in tests. The particle pool and the live counter are in ShaderRead after simulate(); a
    // caller copying them out transitions to CopySource and back.
    [[nodiscard]] rhi::BufferHandle particle_pool(ParticleEmitterId id) const;
    [[nodiscard]] rhi::BufferHandle live_counter(ParticleEmitterId id) const;
    [[nodiscard]] rhi::BufferHandle indirect_arguments(ParticleEmitterId id) const;
    [[nodiscard]] std::size_t emitter_count() const noexcept { return emitters_.size(); }
    [[nodiscard]] std::uint32_t allocated_particles() const noexcept { return allocated_; }

    // Destroys every handle the system owns. The RHI retires the storage behind its fence.
    [[nodiscard]] rhi::RhiStatus release();

private:
    struct Emitter;
    ParticleSystem(rhi::Device& device, ParticleBudget budget) noexcept;
    // Destroys one emitter's handles, in dependency order. The first failure is returned; every handle is still attempted.
    [[nodiscard]] rhi::RhiStatus destroy_emitter(Emitter& emitter);

    rhi::Device& device_;
    ParticleBudget budget_;
    rhi::DescriptorSetLayoutHandle simulate_layout_;
    rhi::DescriptorSetLayoutHandle args_layout_;
    rhi::DescriptorSetLayoutHandle render_layout_;
    rhi::PipelineHandle simulate_pipeline_;
    rhi::PipelineHandle args_pipeline_;
    rhi::PipelineHandle render_pipelines_[2];  // indexed by ParticleBlend
    rhi::BufferHandle quad_indices_;
    rhi::BufferHandle dummy_vertex_;  // bound to slot 0 for the draw: the RHI requires a vertex buffer even with no vertex input
    rhi::BufferHandle zero_;
    rhi::TextureHandle texture_;
    std::unordered_map<ParticleEmitterId, std::unique_ptr<Emitter>> emitters_;
    std::vector<ParticleEmitterId> order_;  // creation order, so the draw order is stable
    ParticleEmitterId next_id_ = 1;
    std::uint32_t allocated_ = 0;
    bool released_ = false;
};

}  // namespace engine::render
