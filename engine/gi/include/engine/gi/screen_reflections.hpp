#pragma once

// Screen-space reflections (US-040), the native profile's reflections on the CPU. A reflected ray marches through the frame's view-space
// depth. A hit returns the lit color already on screen. A miss returns nothing, and the caller uses the environment (IBL) instead. The
// reference matches the GPU pass step for step, so the same frame gives the same answer on both.
//
// Coordinates: view space is right-handed with the eye at the origin looking down -Z. Depth is the positive distance along -Z. The
// projection maps view space to clip space the way glm::perspectiveRH_ZO does, with NDC y up. Pixels have row 0 at the top.
//
// A ray misses when it leaves the screen, goes behind the eye, runs out of steps, or starts on a surface rougher than the cutoff. A hit
// fades out near the screen edges so that a reflection does not pop when it crosses them. Off-screen geometry is never visible to this
// pass, so the fallback is the environment, not black.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace engine::gi {

// What the frame provides to the trace. Both arrays hold width * height entries, row-major from the top row.
struct ScreenBuffers {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> view_depth;     // positive distance from the eye; +infinity where nothing was drawn
    std::vector<glm::vec3> radiance;   // linear color of the lit scene
};

struct ReflectionView {
    glm::mat4 projection{1.0f};  // view space to clip space
};

struct ScreenTraceSettings {
    std::uint32_t max_steps = 256;      // uniform steps along the reflected ray; 256 steps of 0.39 m cover 100 m under the thickness
    float max_distance = 100.0f;        // meters along the reflected ray, matching the full profile's 100 m limit
    float thickness = 0.5f;             // meters: a sample hits when it lies within this depth behind the visible surface
    float edge_fade = 0.1f;             // NDC distance from the screen edge over which hits fade to zero
    float roughness_cutoff = 0.6f;      // rougher surfaces skip the trace and use the environment
};

struct ScreenHit {
    bool hit = false;
    glm::vec3 radiance{0.0f};
    float confidence = 0.0f;  // 1 at the screen center, fading to 0 at the edge; 0 on a miss
};

// Marches the reflected ray from `origin` (view space) along `direction` (unit, view space).
[[nodiscard]] ScreenHit trace_screen_reflection(const ScreenBuffers& buffers, const ReflectionView& view, const glm::vec3& origin,
                                                const glm::vec3& direction, float roughness, const ScreenTraceSettings& settings) noexcept;

// The reflection radiance: the screen hit weighted by its confidence, with the environment for the rest.
[[nodiscard]] glm::vec3 reflection_radiance(const ScreenHit& hit, const glm::vec3& environment) noexcept;

}  // namespace engine::gi
