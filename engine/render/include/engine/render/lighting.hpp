#pragma once

// Direct lighting and shadow planning (US-035). Lights are directional, point, or spot. The renderer sorts them by priority, assigns each
// one to the clusters it touches (a cluster is a screen tile by a depth slice of the camera's view), and uploads the lists the scene shader
// reads. Directional lights get cascaded shadow maps; spot lights get one tile in the local shadow atlas (shadow_atlas.hpp). Point lights
// light the scene but cast no shadows yet (docs/lighting.md lists the scope call).
//
// Everything here runs on the CPU and needs no GPU. The cluster assignment is the golden source for the debug views, and the GPU tests
// compare the shader's per-pixel cluster counts with it. Coordinates are camera-relative, as in the frame snapshot: positions are
// converted from double once, after the camera position is subtracted.

#include "engine/render/camera.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::render {

enum class LightType : std::uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};
[[nodiscard]] const char* to_string(LightType type) noexcept;

// One light as the author describes it. Positions are world-space (double), so they stay precise far from the origin.
struct Light {
    std::uint32_t id = 0;  // stable author id. Shadow tiles persist under it, so it must be unique among the shadow casters
    LightType type = LightType::Point;
    glm::dvec3 position{0.0};             // point and spot
    glm::vec3 direction{0.0f, -1.0f, 0.0f};  // unit; the way the light travels (directional and spot)
    glm::vec3 color{1.0f};                // linear
    float intensity = 1.0f;               // linear scale; a directional light's value is its irradiance on a perpendicular surface
    float range = 10.0f;                  // point and spot: meters beyond which the light contributes nothing
    float inner_angle = 0.0f;             // spot: half-angle (radians) of the full-intensity cone
    float outer_angle = 0.5f;             // spot: half-angle (radians) of the cutoff cone; inner <= outer < pi / 2
    float priority = 0.0f;                // when a cluster overflows, higher priorities are kept
    bool casts_shadow = false;
    // Shadow tuning. depth_bias is subtracted from the receiver's depth in NDC units (0..1); light_bias moves the receiver along its
    // normal, in texels of its shadow map (so it scales with the map's world texel size); pcf_radius is the filter half-width in texels.
    float depth_bias = 0.0005f;
    float light_bias = 1.0f;
    std::uint32_t pcf_radius = 1;
};

// Returns the first problem, or nothing when the light is finite and in range.
[[nodiscard]] std::optional<std::string> validate_light(const Light& light);

// Light budgets and the cluster grid. The grid is screen tiles of tile_px pixels by `slices` depth slices, spaced exponentially between the
// camera's near and far planes. Each cluster holds at most max_lights_per_cluster light indices. Lights beyond that are dropped by priority
// and counted, so an overflow is visible rather than silent.
struct LightingSettings {
    std::uint32_t max_lights = 256;
    std::uint32_t tile_px = 16;
    std::uint32_t slices = 8;
    std::uint32_t max_lights_per_cluster = 32;
};

// Cascade layout of the directional shadow. The split between cascades follows the practical scheme: lambda blends logarithmic and uniform
// splits. Cascades cover the camera's near plane up to shadow_distance (clamped to the far plane). caster_margin extends each cascade's
// depth range toward the light, so casters outside the view still shadow what is visible.
struct CascadeSettings {
    std::uint32_t count = 4;  // 1 to kMaxCascades
    float lambda = 0.6f;
    float shadow_distance = 200.0f;
    float caster_margin = 100.0f;
};
inline constexpr std::uint32_t kMaxCascades = 4;

// The cluster grid for one frame. Everything the shader needs to find a fragment's cluster is here.
struct ClusterGrid {
    std::uint32_t width = 0;   // target size in pixels
    std::uint32_t height = 0;
    std::uint32_t tiles_x = 0;
    std::uint32_t tiles_y = 0;
    std::uint32_t slices = 0;
    std::uint32_t tile_px = 0;
    std::uint32_t max_lights_per_cluster = 0;
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    glm::vec3 forward{0.0f, 0.0f, -1.0f};  // camera forward in camera-relative space; a fragment's view depth is dot(p, forward)

    [[nodiscard]] std::uint32_t cluster_count() const noexcept {
        return tiles_x * tiles_y * slices;
    }
    // The slice a view depth falls in. Depths below the near plane use slice 0, and depths beyond the far plane use the last slice.
    [[nodiscard]] std::uint32_t slice_of(float view_depth) const noexcept;
    // Cluster index of the pixel (x, y), with y down, and of the view depth.
    [[nodiscard]] std::uint32_t cluster_of(std::uint32_t x, std::uint32_t y, float view_depth) const noexcept;
};

// Builds the grid for a target of the given size and a camera. Returns nothing when the settings are unusable (zero sizes, or slices and
// tiles that do not fit the cluster arrays).
[[nodiscard]] std::optional<ClusterGrid> make_cluster_grid(const LightingSettings& settings, const Camera& camera, std::uint32_t width,
                                                           std::uint32_t height);

// One light as the clusterer sees it: a bounding sphere in camera-relative space. Directional lights have no sphere and cover every cluster.
struct ClusteredLight {
    std::uint32_t index = 0;  // the light's slot in the GPU light list
    LightType type = LightType::Point;
    glm::vec3 center{0.0f};   // camera-relative
    float radius = 0.0f;      // reach of the light (range for point and spot)
};

// The cluster lists. counts holds one word per cluster: the number of stored indices in the low 16 bits, and kClusterOverflowBit set when
// some light was dropped from it. indices holds max_lights_per_cluster words per cluster, in cluster order; the first counts[c] are used.
inline constexpr std::uint32_t kClusterOverflowBit = 1u << 16;
struct ClusterAssignment {
    std::vector<std::uint32_t> counts;
    std::vector<std::uint32_t> indices;
    std::uint32_t overflow_clusters = 0;  // clusters that dropped at least one light
    std::uint32_t dropped_entries = 0;    // light-cluster pairs that did not fit
};

// Assigns lights to clusters. `lights` must already be in priority order (highest first), so a full cluster keeps the lights that come first.
// The screen footprint of a sphere is the projected bounds of its box, which contains the sphere's projection; a sphere that reaches the
// near plane or behind the eye covers every column of its slices. `view_projection` is the engine (DX-convention) matrix of the snapshot.
[[nodiscard]] ClusterAssignment assign_clusters(const ClusterGrid& grid, const glm::mat4& view_projection,
                                                const std::vector<ClusteredLight>& lights);

// Light falloff, as the shader computes it: inverse-square intensity with a smooth window at the range for point and spot lights, and a
// squared smoothstep cone for spot lights. Directional lights are unattenuated. Returns the scale to apply to the light's color.
[[nodiscard]] float light_attenuation(const Light& light, float distance, float cos_angle_to_axis) noexcept;

// One cascade of a directional shadow: the split it ends at (view distance from the eye) and its light-space matrix, camera-relative and
// in engine (DX-convention) clip space. Cascades are ordered from the nearest.
struct CascadePlan {
    float split_far = 0.0f;
    float radius = 0.0f;  // the cascade sphere's radius, which is half the light box's width
    glm::mat4 view_projection{1.0f};
};

// Fits the cascades of a directional light to the camera's frustum. Each cascade is a sphere around its slice of the frustum, so its
// size does not change as the camera turns. The light's view centers on the sphere's texel grid, which keeps shadows from shimmering.
[[nodiscard]] std::vector<CascadePlan> plan_cascades(const Camera& camera, const glm::vec3& light_direction, const CascadeSettings& settings,
                                                     std::uint32_t resolution);

// Light-space matrix of a spot light, camera-relative and in engine clip space. The cone's cutoff is its perspective field of view.
[[nodiscard]] glm::mat4 spot_view_projection(const glm::vec3& position, const glm::vec3& direction, float outer_angle, float range) noexcept;

}  // namespace engine::render
