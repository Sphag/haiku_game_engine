#pragma once

// Baked diffuse irradiance probes (US-040), the native profile's GI. The converter bakes a grid of probes from an analytic sky and a set
// of axis-aligned occluders. The runtime reads the grid back, interpolates it at a point, and falls back to the environment where the grid
// does not cover the point or where its probes are not usable.
//
// A probe stores nine spherical-harmonic coefficients in the same convention as Environment::diffuse_sh (irradiance divided by pi), so
// evaluate_irradiance() gives the same value as evaluate_diffuse() for an environment with those coefficients. Diffuse shading is then
// albedo * value. Bounced light is one bounce: a ray that hits an occluder takes the occluder's emission plus its albedo times the sky
// irradiance at the hit point, and that sky term is zero when a ray along the hit normal is blocked. Specular comes from SSR and IBL
// (screen_reflections.hpp).
//
// Everything here is CPU-only and deterministic. The serialized volume carries a format version and the hash of the bake inputs, so a
// reader can tell when the data is stale. Probes are in one of three states: Unbaked (a budgeted bake has not reached them yet), Blocked
// (inside an occluder or on its surface, so no free-space light), and Valid.

#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::gi {

// Version of the serialized volume and of the bake's math. A reader rejects any other value.
inline constexpr std::uint32_t kFormatVersion = 1;
// Upper bound on the probe count of one volume. It keeps a bad source from asking for an enormous allocation.
inline constexpr std::size_t kMaxProbes = std::size_t{1} << 20;

// Nine real SH coefficients, bands 0 to 2, in the order of Environment::diffuse_sh.
using Sh9 = std::array<glm::vec3, 9>;
[[nodiscard]] std::array<float, 9> sh_basis(const glm::vec3& direction) noexcept;
// Irradiance divided by pi at a unit normal, from SH coefficients stored in the diffuse convention.
[[nodiscard]] glm::vec3 evaluate_irradiance(const Sh9& sh, const glm::vec3& normal) noexcept;

// The analytic sky that lights the bake and provides the fallback. Radiance is a vertical gradient from the horizon to the zenith, the
// ground color below the horizon, and an optional sun lobe of about 3 degrees in radius.
struct Sky {
    glm::vec3 zenith{0.35f, 0.55f, 0.95f};
    glm::vec3 horizon{0.75f, 0.80f, 0.85f};
    glm::vec3 ground{0.12f, 0.10f, 0.08f};
    glm::vec3 sun_direction{0.0f, 1.0f, 0.0f};  // unit, toward the sun
    glm::vec3 sun_radiance{0.0f};               // linear; zero for no sun
};

[[nodiscard]] glm::vec3 sky_radiance(const Sky& sky, const glm::vec3& direction) noexcept;
// Projects the sky onto SH with a fixed Fibonacci sampling of the sphere, so the result is the same on every run.
[[nodiscard]] Sh9 project_sky(const Sky& sky) noexcept;

// An axis-aligned occluder. It blocks rays and is lit by the sky, and it emits `emitted` when a ray reaches it. min < max on each axis.
struct Box {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    glm::vec3 albedo{0.5f};
    glm::vec3 emitted{0.0f};
};

// The probe grid. Probe (x, y, z) sits at origin + spacing * (x, y, z). Index order is x fastest, then y, then z.
struct GridLayout {
    glm::vec3 origin{0.0f};
    float spacing = 1.0f;
    glm::uvec3 dims{1u};
};

// Everything the bake depends on. Two equal inputs bake to equal bytes.
struct BakeInputs {
    GridLayout grid;
    Sky sky;
    std::vector<Box> boxes;
    std::uint32_t samples = 256;         // directions per probe; the sky projection uses at least as many
    float max_ray_distance = 1000.0f;    // a ray that finds nothing within this distance sees the sky
};

enum class ProbeState : std::uint8_t {
    Unbaked = 0,
    Valid = 1,
    Blocked = 2,
};

struct Probe {
    Sh9 sh{};
    ProbeState state = ProbeState::Unbaked;
};

struct GiVolume {
    std::uint32_t format_version = kFormatVersion;
    std::uint64_t source_hash = 0;  // hash_inputs() of the bake that produced the probes
    GridLayout grid;
    std::vector<Probe> probes;
};

// Returns the first problem, or nothing when the inputs can be baked: a non-empty grid within kMaxProbes, positive finite spacing, at
// least one sample, boxes with min < max, and finite colors.
[[nodiscard]] std::optional<std::string> validate_inputs(const BakeInputs& inputs);

// FNV-1a over every field of the inputs, in a fixed order. Any change to a field changes the hash.
[[nodiscard]] std::uint64_t hash_inputs(const BakeInputs& inputs) noexcept;

// A volume with every probe Unbaked and its source hash set. The inputs must validate.
[[nodiscard]] GiVolume create_volume(const BakeInputs& inputs);
// Bakes up to `budget` unbaked probes in index order and returns how many it baked. Returns 0 when the volume does not belong to these
// inputs (is_current() is false), so stale data is never filled in with new math. Probes that are already baked are not changed.
[[nodiscard]] std::size_t bake_probes(GiVolume& volume, const BakeInputs& inputs, std::size_t budget);
// create_volume() followed by a bake of every probe.
[[nodiscard]] GiVolume bake_volume(const BakeInputs& inputs);

// True when the volume was made by this format version from these inputs. Use it to decide whether cached data must be rebaked.
[[nodiscard]] bool is_current(const GiVolume& volume, const BakeInputs& inputs) noexcept;

// Little-endian binary: magic "OWGI", format version, source hash, grid, then one state byte and nine RGB floats per probe.
[[nodiscard]] std::string serialize(const GiVolume& volume);
[[nodiscard]] core::Result<GiVolume, std::string> deserialize(std::string_view bytes);

// The result of one lookup. `irradiance` is in the diffuse convention (irradiance / pi). `coverage` is the share of the trilinear weight
// that landed on baked, valid probes, from 0 to 1. The rest of the value comes from the fallback environment.
struct IrradianceSample {
    glm::vec3 irradiance{0.0f};
    float coverage = 0.0f;
};

// Trilinear interpolation of the probes around `position`. Blocked and unbaked probes take no weight, and the remaining weight is
// renormalized. Where the valid weight is below one, the rest of the answer is the fallback environment evaluated at `normal`. Outside the
// grid the answer is entirely the fallback, with coverage zero.
[[nodiscard]] IrradianceSample sample_irradiance(const GiVolume& volume, const glm::vec3& position, const glm::vec3& normal,
                                                 const Sh9& fallback) noexcept;

// Debug view of probe coverage. Each position is classified by its nearest probe: green for Valid, red for Blocked, yellow for Unbaked, and
// dark gray outside the grid.
[[nodiscard]] glm::vec3 coverage_debug_color(const GiVolume& volume, const glm::vec3& position) noexcept;

}  // namespace engine::gi
