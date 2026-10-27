#include "engine/render/lighting.hpp"

#include "engine/core/coordinates.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {

// Largest cluster index list the 16-bit count field can describe.
constexpr std::uint32_t kMaxLightsPerCluster = 0xFFFF;

bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Projects a camera-relative point through the snapshot's matrix to pixel coordinates (y down, origin at the top left). Returns false when
// the point is at or behind the eye.
bool to_pixel(const glm::mat4& view_projection, const glm::vec3& p, float width, float height, glm::vec2& pixel) {
    const glm::vec4 clip = view_projection * glm::vec4(p, 1.0f);
    if (clip.w <= 0.0f) {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    pixel = glm::vec2((ndc.x * 0.5f + 0.5f) * width, (1.0f - ndc.y) * 0.5f * height);
    return true;
}

float spot_cos(float angle) {
    return std::cos(angle);
}

// The corners of the sphere's bounding box, in camera-relative space.
void box_corners(const glm::vec3& center, float radius, glm::vec3 (&corners)[8]) {
    int index = 0;
    for (const float sx : {-1.0f, 1.0f}) {
        for (const float sy : {-1.0f, 1.0f}) {
            for (const float sz : {-1.0f, 1.0f}) {
                corners[index++] = center + glm::vec3(sx, sy, sz) * radius;
            }
        }
    }
}

}  // namespace

const char* to_string(LightType type) noexcept {
    switch (type) {
        case LightType::Directional:
            return "directional";
        case LightType::Point:
            return "point";
        case LightType::Spot:
            return "spot";
    }
    return "unknown";
}

std::optional<std::string> validate_light(const Light& light) {
    if (!finite(light.color) || light.color.r < 0.0f || light.color.g < 0.0f || light.color.b < 0.0f) {
        return std::string("light color must be finite and non-negative");
    }
    if (!std::isfinite(light.intensity) || light.intensity < 0.0f) {
        return std::string("light intensity must be finite and non-negative");
    }
    if (!std::isfinite(light.priority)) {
        return std::string("light priority must be finite");
    }
    if (light.type != LightType::Directional) {
        if (!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z)) {
            return std::string("light position must be finite");
        }
        if (!(light.range > 0.0f) || !std::isfinite(light.range)) {
            return std::string("point and spot lights need a finite range above zero");
        }
    }
    if (light.type != LightType::Point) {
        if (!finite(light.direction) || std::abs(glm::length(light.direction) - 1.0f) > 1e-4f) {
            return std::string("light direction must be a finite unit vector");
        }
    }
    if (light.type == LightType::Spot) {
        const float pi_half = 1.5707963267948966f;
        if (!(light.inner_angle >= 0.0f) || !(light.outer_angle < pi_half) || !(light.inner_angle <= light.outer_angle) ||
            !(light.outer_angle > 0.0f)) {
            return std::string("spot angles need 0 <= inner <= outer < pi / 2 with outer above zero");
        }
    }
    if (!std::isfinite(light.depth_bias) || light.depth_bias < 0.0f || light.depth_bias > 0.1f) {
        return std::string("depth bias must be in [0, 0.1]");
    }
    if (!std::isfinite(light.light_bias) || light.light_bias < 0.0f || light.light_bias > 4.0f) {
        return std::string("normal bias must be in [0, 4] texels");
    }
    if (light.pcf_radius > 4) {
        return std::string("PCF radius must be at most 4 texels");
    }
    return std::nullopt;
}

std::uint32_t ClusterGrid::slice_of(float view_depth) const noexcept {
    if (!(view_depth > near_plane) || slices <= 1) {
        return 0;
    }
    const float ratio = std::log(view_depth / near_plane) / std::log(far_plane / near_plane);
    const auto slice = static_cast<std::uint32_t>(std::floor(ratio * static_cast<float>(slices)));
    return std::min(slice, slices - 1);
}

std::uint32_t ClusterGrid::cluster_of(std::uint32_t x, std::uint32_t y, float view_depth) const noexcept {
    const std::uint32_t tx = std::min(x / tile_px, tiles_x - 1);
    const std::uint32_t ty = std::min(y / tile_px, tiles_y - 1);
    return (slice_of(view_depth) * tiles_y + ty) * tiles_x + tx;
}

std::optional<ClusterGrid> make_cluster_grid(const LightingSettings& settings, const Camera& camera, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || settings.tile_px == 0 || settings.slices == 0 || settings.max_lights_per_cluster == 0 ||
        settings.max_lights_per_cluster > kMaxLightsPerCluster || settings.max_lights == 0 || !camera.is_valid()) {
        return std::nullopt;
    }
    ClusterGrid grid;
    grid.width = width;
    grid.height = height;
    grid.tile_px = settings.tile_px;
    grid.tiles_x = (width + settings.tile_px - 1) / settings.tile_px;
    grid.tiles_y = (height + settings.tile_px - 1) / settings.tile_px;
    grid.slices = settings.slices;
    grid.max_lights_per_cluster = settings.max_lights_per_cluster;
    grid.near_plane = static_cast<float>(camera.near_plane);
    grid.far_plane = static_cast<float>(camera.far_plane);
    grid.forward = glm::vec3(camera.forward());
    // A grid with more clusters than the GPU can index in 32 bits is not useful; the realistic ceiling is far below this.
    const std::uint64_t entries = static_cast<std::uint64_t>(grid.cluster_count()) * settings.max_lights_per_cluster;
    if (entries > (1u << 26)) {
        return std::nullopt;
    }
    return grid;
}

ClusterAssignment assign_clusters(const ClusterGrid& grid, const glm::mat4& view_projection, const std::vector<ClusteredLight>& lights) {
    ClusterAssignment out;
    const std::uint32_t clusters = grid.cluster_count();
    const std::uint32_t capacity = grid.max_lights_per_cluster;
    out.counts.assign(clusters, 0);
    out.indices.assign(static_cast<std::size_t>(clusters) * capacity, 0);
    const float width = static_cast<float>(grid.width);
    const float height = static_cast<float>(grid.height);
    const float tile = static_cast<float>(grid.tile_px);

    for (const ClusteredLight& light : lights) {
        std::uint32_t slice_lo = 0;
        std::uint32_t slice_hi = grid.slices - 1;
        std::uint32_t tx_lo = 0;
        std::uint32_t tx_hi = grid.tiles_x - 1;
        std::uint32_t ty_lo = 0;
        std::uint32_t ty_hi = grid.tiles_y - 1;

        if (light.type != LightType::Directional) {
            const float depth = glm::dot(light.center, grid.forward);
            const float z_min = depth - light.radius;
            const float z_max = depth + light.radius;
            if (z_max < grid.near_plane) {
                continue;  // wholly between the eye and the near plane: not visible
            }
            slice_lo = grid.slice_of(std::max(z_min, grid.near_plane));
            slice_hi = grid.slice_of(z_max);
            if (z_min > grid.near_plane) {
                // The box is in front of the near plane, so its projection bounds the sphere's footprint.
                glm::vec3 corners[8];
                box_corners(light.center, light.radius, corners);
                glm::vec2 lo(std::numeric_limits<float>::max());
                glm::vec2 hi(-std::numeric_limits<float>::max());
                bool visible = true;
                for (const glm::vec3& corner : corners) {
                    glm::vec2 pixel;
                    if (!to_pixel(view_projection, corner, width, height, pixel)) {
                        visible = false;
                        break;
                    }
                    lo = glm::min(lo, pixel);
                    hi = glm::max(hi, pixel);
                }
                if (visible) {
                    if (hi.x < 0.0f || hi.y < 0.0f || lo.x >= width || lo.y >= height) {
                        continue;  // off the screen
                    }
                    tx_lo = static_cast<std::uint32_t>(std::max(0.0f, std::floor(lo.x / tile)));
                    ty_lo = static_cast<std::uint32_t>(std::max(0.0f, std::floor(lo.y / tile)));
                    tx_hi = std::min(grid.tiles_x - 1, static_cast<std::uint32_t>(std::max(0.0f, std::floor(hi.x / tile))));
                    ty_hi = std::min(grid.tiles_y - 1, static_cast<std::uint32_t>(std::max(0.0f, std::floor(hi.y / tile))));
                }
            }
        }

        for (std::uint32_t slice = slice_lo; slice <= slice_hi; ++slice) {
            for (std::uint32_t ty = ty_lo; ty <= ty_hi; ++ty) {
                for (std::uint32_t tx = tx_lo; tx <= tx_hi; ++tx) {
                    const std::uint32_t cluster = (slice * grid.tiles_y + ty) * grid.tiles_x + tx;
                    const std::uint32_t count = out.counts[cluster] & 0xFFFFu;
                    if (count < capacity) {
                        out.indices[static_cast<std::size_t>(cluster) * capacity + count] = light.index;
                        out.counts[cluster] = count + 1;
                    } else {
                        if ((out.counts[cluster] & kClusterOverflowBit) == 0) {
                            out.counts[cluster] |= kClusterOverflowBit;
                            ++out.overflow_clusters;
                        }
                        ++out.dropped_entries;
                    }
                }
            }
        }
    }
    return out;
}

float light_attenuation(const Light& light, float distance, float cos_angle_to_axis) noexcept {
    if (light.type == LightType::Directional) {
        return light.intensity;
    }
    // Window at the range: (1 - (d / r)^4)^2, clamped, so the light fades to zero exactly at its range.
    const float ratio = distance / light.range;
    const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
    float scale = light.intensity * window * window / std::max(distance * distance, 0.01f);
    if (light.type == LightType::Spot) {
        const float cos_inner = spot_cos(light.inner_angle);
        const float cos_outer = spot_cos(light.outer_angle);
        float cone = cos_angle_to_axis >= cos_outer ? 1.0f : 0.0f;
        if (cos_inner > cos_outer) {
            const float t = std::clamp((cos_angle_to_axis - cos_outer) / (cos_inner - cos_outer), 0.0f, 1.0f);
            cone = t * t;
        }
        scale *= cone;
    }
    return scale;
}

std::vector<CascadePlan> plan_cascades(const Camera& camera, const glm::vec3& light_direction, const CascadeSettings& settings,
                                       std::uint32_t resolution) {
    std::vector<CascadePlan> plans;
    if (!camera.is_valid() || resolution == 0 || settings.count == 0 || settings.count > kMaxCascades || !finite(light_direction) ||
        std::abs(glm::length(light_direction) - 1.0f) > 1e-4f) {
        return plans;
    }
    const float near = static_cast<float>(camera.near_plane);
    const float far = std::min(static_cast<float>(camera.far_plane), settings.shadow_distance);
    if (!(far > near)) {
        return plans;
    }
    const glm::vec3 eye(0.0f);  // camera-relative
    const glm::vec3 forward = glm::vec3(camera.forward());
    const glm::quat orientation = glm::quat(static_cast<float>(camera.orientation.w), static_cast<float>(camera.orientation.x),
                                            static_cast<float>(camera.orientation.y), static_cast<float>(camera.orientation.z));
    const glm::vec3 right = orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 up = orientation * glm::vec3(0.0f, 1.0f, 0.0f);
    const float tan_half = std::tan(static_cast<float>(camera.fov_y_radians) * 0.5f);
    const float aspect = static_cast<float>(camera.aspect);
    const std::uint32_t count = settings.count;

    // The practical split scheme: a blend of logarithmic and uniform splits.
    std::vector<float> splits(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(count);
        const float logarithmic = near * std::pow(far / near, p);
        const float uniform = near + (far - near) * p;
        splits[i] = settings.lambda * logarithmic + (1.0f - settings.lambda) * uniform;
    }
    splits.back() = far;

    // Light-space up vector: any vector not parallel to the light direction.
    const glm::vec3 light_up = std::abs(light_direction.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const float texel_scale = static_cast<float>(resolution);
    float slice_near = near;
    for (std::uint32_t i = 0; i < count; ++i) {
        const float slice_far = splits[i];
        // The slice's eight corners, at both ends of its depth range.
        glm::vec3 corners[8];
        int index = 0;
        for (const float depth : {slice_near, slice_far}) {
            const float half_h = tan_half * depth;
            const float half_w = half_h * aspect;
            const glm::vec3 axis = eye + forward * depth;
            for (const float sx : {-1.0f, 1.0f}) {
                for (const float sy : {-1.0f, 1.0f}) {
                    corners[index++] = axis + right * (sx * half_w) + up * (sy * half_h);
                }
            }
        }
        // The bounding sphere is centered on the slice's axis at mid depth, so its size depends on the slice and not on the camera's
        // orientation. The radius rounds up to 1/16 m so small camera motion does not change it.
        const glm::vec3 center = eye + forward * (0.5f * (slice_near + slice_far));
        float radius = 0.0f;
        for (const glm::vec3& corner : corners) {
            radius = std::max(radius, glm::length(corner - center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const float distance = radius + settings.caster_margin;
        const glm::vec3 light_eye = center - light_direction * distance;
        glm::mat4 view = glm::lookAtRH(light_eye, center, light_up);

        // Snap the center to the texel grid of the light's view, so the shadow map moves in whole texels.
        const float texel = 2.0f * radius / texel_scale;
        const glm::vec3 center_light = glm::vec3(view * glm::vec4(center, 1.0f));
        const glm::vec2 snapped = glm::round(glm::vec2(center_light) / texel) * texel;
        const glm::vec2 delta = snapped - glm::vec2(center_light);
        view = glm::translate(glm::mat4(1.0f), glm::vec3(delta.x, delta.y, 0.0f)) * view;

        const glm::mat4 projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, distance + radius);
        CascadePlan plan;
        plan.split_far = slice_far;
        plan.radius = radius;
        plan.view_projection = projection * view;
        plans.push_back(plan);
        slice_near = slice_far;
    }
    return plans;
}

glm::mat4 spot_view_projection(const glm::vec3& position, const glm::vec3& direction, float outer_angle, float range) noexcept {
    const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 view = glm::lookAtRH(position, position + direction, up);
    const float near = std::max(0.02f, range * 0.002f);
    const glm::mat4 projection = core::coords::perspective(2.0f * outer_angle, 1.0f, near, range);
    return projection * view;
}

}  // namespace engine::render
