#pragma once

// Dynamic probe-grid GI (US-044): the bookkeeping of the ray-traced diffuse probe grid that the render pass (engine/render/dynamic_gi.hpp)
// traces and blends. The grid is toroidal. Probe slot s holds the cell whose index is congruent to s on every axis, so a camera move reuses
// every probe whose cell stays inside the grid, and resets only the plane of cells that enters it. Each frame the grid plans a budget of
// probes in round-robin order. A probe's blend weight is 1 / (1 + samples), so its history is a running average of its traces until
// max_history, and after that it keeps a fixed rate. A lighting change keeps only lighting_history samples per probe, so the bounce follows
// the light within a few frames. A teleport resets everything, and a streaming change resets the probes inside the changed region.
//
// Probe (cell + 0.5) * spacing is the world position of a cell's probe. The half cell keeps probes out of the planes of the world's cell
// boundaries. Sampling reuses the baked volume's trilinear code (probes.hpp), so the native and dynamic paths share one convention: the
// history holds nine SH coefficients in the diffuse convention, and to_volume() lays them out as a GiVolume.
//
// Everything here is CPU-only and deterministic. The GPU pass owns the history bytes; this class owns the cells, the sample counts, and the
// plan, and it never sees the coefficients except when it builds a volume for sampling.

#include "engine/core/result.hpp"
#include "engine/gi/probes.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::gi {

// Upper bounds that keep a bad description from asking for an enormous grid or ray count.
inline constexpr std::size_t kMaxDynamicProbes = std::size_t{1} << 16;
inline constexpr std::uint32_t kMaxDynamicRays = 4096;

struct DynamicGridDesc {
    glm::uvec3 dims{32u, 8u, 32u};     // probes per axis; the full profile's grid (PRD, Image Profiles)
    float spacing = 4.0f;              // meters between probes; the full profile's spacing
    std::uint32_t rays_per_probe = 64; // the full profile's ray count
    std::uint32_t update_budget = 512; // probes traced per frame: 1/16 of the grid in the full profile
    std::uint32_t max_history = 64;    // samples a probe averages before its blend weight stops falling
    std::uint32_t lighting_history = 4; // samples a probe keeps when the lighting changes
};

// The lighting the traces see. Radiance is linear, in the units the render pass uses. The sun direction points from the surface to the sun.
struct DynamicLighting {
    glm::vec3 sun_direction{0.0f, 1.0f, 0.0f};
    glm::vec3 sun_radiance{0.0f};
    glm::vec3 sky_radiance{0.0f};  // uniform sky: what a ray that misses the scene brings back
    [[nodiscard]] bool same_as(const DynamicLighting& other) const noexcept {
        return sun_direction == other.sun_direction && sun_radiance == other.sun_radiance && sky_radiance == other.sky_radiance;
    }
};

// Returns the first problem, or nothing when the description can build a grid: non-empty dims within kMaxDynamicProbes, positive finite
// spacing, rays in [1, kMaxDynamicRays], a budget in [1, probe count], a history of at least one sample, and a lighting history that does
// not exceed the maximum history.
[[nodiscard]] std::optional<std::string> validate_desc(const DynamicGridDesc& desc);

// One probe the next frame traces. The weight blends the new trace into the history: history = mix(history, trace, weight).
struct ProbeUpdate {
    std::uint32_t slot = 0;
    float weight = 1.0f;
    glm::ivec3 cell{0};
};

class DynamicGrid {
public:
    // Builds a grid centred on the origin, with no samples. The description must validate.
    [[nodiscard]] static core::Result<DynamicGrid, std::string> create(const DynamicGridDesc& desc);

    [[nodiscard]] const DynamicGridDesc& desc() const noexcept { return desc_; }
    [[nodiscard]] std::size_t probe_count() const noexcept { return cells_.size(); }
    [[nodiscard]] const DynamicLighting& lighting() const noexcept { return lighting_; }

    // Moves the grid so it is centred on the eye, in whole cells. Probes whose cell changed lose their history. Returns how many did.
    std::size_t follow(const glm::vec3& eye);
    // Discards the history of every probe (a teleport). The grid keeps its position and the round-robin cursor.
    void reset() noexcept;
    // Discards the history of the probes whose positions lie in [min, max] on every axis (a streaming change). Returns how many probes lie
    // inside the region, whether or not they had samples.
    std::size_t reset_region(const glm::vec3& min, const glm::vec3& max);
    // Records the lighting. Returns true when it differs from the last one, and then cuts every probe's history to lighting_history.
    bool set_lighting(const DynamicLighting& lighting);

    // Plans the next update_budget probes in round-robin order. Planning does not change the samples; commit() does, once the trace is
    // recorded. Every planned weight comes from the samples before the plan, so a plan can be discarded without side effects.
    [[nodiscard]] std::vector<ProbeUpdate> plan_updates();
    // Counts one trace for each planned probe. A probe's samples saturate at max_history.
    void commit(const std::vector<ProbeUpdate>& updates) noexcept;

    // The probe's samples (traces averaged into its history). Zero means its history is not valid yet. Out-of-range slots return zero.
    [[nodiscard]] std::uint32_t samples(std::uint32_t slot) const noexcept;
    // The absolute cell that a slot currently holds.
    [[nodiscard]] glm::ivec3 cell(std::uint32_t slot) const noexcept;
    // World position of the probe in a slot, at the centre of its cell.
    [[nodiscard]] glm::vec3 probe_position(std::uint32_t slot) const noexcept;
    // The lowest cell of the window [origin, origin + dims) on each axis.
    [[nodiscard]] glm::ivec3 origin() const noexcept { return origin_; }

    // The grid as a volume for sampling: the coefficients of each slot (history, one entry per slot in slot order), with probes that have
    // no samples marked Unbaked, so sample_irradiance() takes no weight from them. A history of the wrong size gives an all-Unbaked volume.
    [[nodiscard]] GiVolume to_volume(const std::vector<Sh9>& history) const;

private:
    explicit DynamicGrid(const DynamicGridDesc& desc);
    [[nodiscard]] glm::ivec3 cell_of_slot(std::uint32_t slot, const glm::ivec3& origin) const noexcept;

    DynamicGridDesc desc_;
    glm::ivec3 origin_{0};  // the lowest cell of the grid on each axis
    DynamicLighting lighting_;
    std::vector<glm::ivec3> cells_;
    std::vector<std::uint32_t> samples_;
    std::uint32_t cursor_ = 0;
};

}  // namespace engine::gi
