#pragma once

// Dynamic ray-traced GI (US-044): the full profile's probe grid, traced on the GPU. DynamicGi owns a gi::DynamicGrid (the camera-following
// cells, sample counts, and round-robin plan, in engine_gi) and a compute pass (shaders/dynamic_gi.hlsl). Each frame:
//   1. grid().follow(eye) moves the grid, and grid().set_lighting(lighting) records the lighting. Both are CPU state.
//   2. update_frame() plans the budget of probes and uploads the parameters and the plan.
//   3. The caller builds the top-level structure (as for RtEffects) and calls record(), which dispatches one thread per planned probe and
//      commits the plan to the grid.
// A planned probe traces rays_per_probe directions on a spherical Fibonacci set from its cell centre. A ray that misses the scene brings
// back the sky. A ray that hits a surface brings back the surface's albedo times the sky plus the sun, with a shadow ray for the sun. The
// radiance is projected onto nine SH coefficients in the diffuse convention of gi/probes.hpp, and the history blends the projection with
// the planned weight. The history is a persistent buffer with one slot per grid slot, zero-filled at creation.
//
// Limits: every triangle is opaque to these rays (RAY_FLAG_FORCE_OPAQUE), so an alpha-masked surface blocks light as if it were solid.
// A probe placed inside solid geometry is not detected; its trace starts inside the surface. Only one bounce is traced, and the bounce's
// hits are shaded with the sky and the sun, not with the grid itself.

#include "engine/gi/dynamic_grid.hpp"
#include "engine/render/rt_effects.hpp"
#include "engine/rhi/command_list.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace engine::render {

// Sizes of the GPU records the pass reads and writes. The byte layouts are documented in shaders/dynamic_gi.hlsl.
inline constexpr std::uint32_t kDynamicGiParameterBytes = 80;
inline constexpr std::uint32_t kDynamicGiUpdateStride = 32;
inline constexpr std::uint32_t kDynamicGiProbeStride = 144;  // nine float4 coefficients
// The sampling block the scene reads (sampling()): a 48-byte header, then one uint per slot that is 1 where the slot holds a trace.
inline constexpr std::uint32_t kDynamicGiSamplingHeaderBytes = 48;
// Threads per group of the pass. The dispatch rounds the planned count up to whole groups.
inline constexpr std::uint32_t kDynamicGiGroupSize = 64;

// What the pass needs beyond the grid: the distance of the sun's shadow rays and the offset of their origin along the surface normal.
struct DynamicGiSettings {
    float shadow_distance = 1000.0f;  // meters
    float bias = 0.01f;               // meters along the normal
};

// The full profile's grid (PRD, Image Profiles): 32 x 8 x 32 probes at 4 m, 64 rays per probe, and 1/16 of the grid updated per frame.
[[nodiscard]] gi::DynamicGridDesc full_profile_grid();

// The byte images the pass uploads. The update count is the planned probes, and the records follow the plan's order. The camera's world
// position is where the traced scene's origin sits: the scene's geometry is camera-relative, so a probe at world position p is traced from
// p - camera_world (the parameters carry it at byte 64).
[[nodiscard]] std::vector<std::uint8_t> pack_dynamic_parameters(const gi::DynamicGridDesc& grid, const gi::DynamicLighting& lighting,
                                                                const DynamicGiSettings& settings, std::uint32_t update_count,
                                                                const glm::vec3& camera_world = glm::vec3(0.0f));
[[nodiscard]] std::vector<std::uint8_t> pack_dynamic_updates(const std::vector<gi::ProbeUpdate>& updates);
// The sampling block for a grid (see sampling()): the window's origin and residue, the spacing and dims, enabled, and the valid flags.
[[nodiscard]] std::vector<std::uint8_t> pack_dynamic_sampling(const gi::DynamicGrid& grid);

// The pass on a device. create() needs the RayTracing feature, and it fails with FeatureDisabled without it. Scene and frame uploads go
// through the render graph's execute, which waits, so call them between frames.
class DynamicGi {
public:
    [[nodiscard]] static rhi::RhiResult<std::unique_ptr<DynamicGi>> create(rhi::Device& device, const gi::DynamicGridDesc& desc, rhi::ShaderBinary shader,
                                                                          DynamicGiSettings settings = {});
    DynamicGi(const DynamicGi&) = delete;
    DynamicGi& operator=(const DynamicGi&) = delete;
    ~DynamicGi() = default;

    // Binds the scene's tables and the top-level structure. The bottom-level structures and the top level are the caller's, as for RtEffects.
    [[nodiscard]] rhi::RhiStatus set_scene(const RtScene& scene);

    [[nodiscard]] gi::DynamicGrid& grid() noexcept { return grid_; }
    [[nodiscard]] const gi::DynamicGrid& grid() const noexcept { return grid_; }
    // The probes the last update_frame() planned, in the order the pass traces them.
    [[nodiscard]] const std::vector<gi::ProbeUpdate>& planned() const noexcept { return plan_; }

    // Plans the frame's probes from the grid and uploads the parameters, the plan, and the sampling block. Needs a scene. camera_world is
    // the camera's world position (the scene's origin); the scene's probes are placed relative to it.
    [[nodiscard]] rhi::RhiStatus update_frame(const glm::vec3& camera_world = glm::vec3(0.0f));
    // Records the dispatch and commits the plan to the grid. Needs a scene, an update_frame() since the last record, and a top-level
    // structure that the list built before this call. The history starts and ends in ShaderRead.
    [[nodiscard]] rhi::RhiStatus record(rhi::CommandList& list);

    // The history buffer: nine float4 per slot, in slot order. The shaders read it, and a copy can read it back.
    [[nodiscard]] rhi::BufferHandle history() const noexcept { return history_; }
    // The sampling block (pack_dynamic_sampling): read by scene.hlsl with the history. It is in ShaderRead after create() and after each
    // update_frame(), so a shader may read it at any point of a frame.
    [[nodiscard]] rhi::BufferHandle sampling() const noexcept { return sampling_; }
    [[nodiscard]] std::uint32_t slot_count() const noexcept { return static_cast<std::uint32_t>(grid_.probe_count()); }

    // Destroys every handle the pass owns. The RHI retires the storage behind its fence.
    [[nodiscard]] rhi::RhiStatus release();

private:
    DynamicGi(rhi::Device& device, gi::DynamicGrid grid, DynamicGiSettings settings) noexcept;

    rhi::Device& device_;
    gi::DynamicGrid grid_;
    DynamicGiSettings settings_;
    rhi::DescriptorSetLayoutHandle layout_;
    rhi::DescriptorSetHandle set_;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle parameters_;
    rhi::BufferHandle updates_;
    rhi::BufferHandle history_;
    rhi::BufferHandle sampling_;
    rhi::BufferHandle instances_;
    rhi::BufferHandle triangles_;
    rhi::ResourceState history_state_ = rhi::ResourceState::Common;
    std::vector<gi::ProbeUpdate> plan_;
    bool parameters_uploaded_ = false;  // the first upload starts from Common, later ones from ShaderRead
    bool updates_uploaded_ = false;
    bool sampling_uploaded_ = false;
    bool scene_set_ = false;
    bool planned_ = false;
};

}  // namespace engine::render
