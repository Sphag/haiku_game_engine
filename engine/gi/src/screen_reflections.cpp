#include "engine/gi/screen_reflections.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace engine::gi {

ScreenHit trace_screen_reflection(const ScreenBuffers& buffers, const ReflectionView& view, const glm::vec3& origin, const glm::vec3& direction,
                                  float roughness, const ScreenTraceSettings& settings) noexcept {
    ScreenHit miss;
    const std::size_t pixel_count = static_cast<std::size_t>(buffers.width) * buffers.height;
    if (pixel_count == 0 || buffers.view_depth.size() != pixel_count || buffers.radiance.size() != pixel_count || settings.max_steps == 0 ||
        roughness > settings.roughness_cutoff) {
        return miss;
    }

    const float step = settings.max_distance / static_cast<float>(settings.max_steps);
    const float width = static_cast<float>(buffers.width);
    const float height = static_cast<float>(buffers.height);
    for (std::uint32_t i = 1; i <= settings.max_steps; ++i) {
        const glm::vec3 point = origin + direction * (step * static_cast<float>(i));
        const float ray_depth = -point.z;
        if (ray_depth <= 0.0f) {
            return miss;  // the ray turned back toward the eye's plane
        }
        const glm::vec4 clip = view.projection * glm::vec4(point, 1.0f);
        if (clip.w <= 0.0f) {
            return miss;
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (std::fabs(ndc.x) > 1.0f || std::fabs(ndc.y) > 1.0f) {
            return miss;  // the ray left the screen, where the frame holds no information
        }
        const auto column = static_cast<std::size_t>(std::min(std::floor((ndc.x * 0.5f + 0.5f) * width), width - 1.0f));
        const auto row = static_cast<std::size_t>(std::min(std::floor((1.0f - (ndc.y * 0.5f + 0.5f)) * height), height - 1.0f));
        const std::size_t index = row * buffers.width + column;
        const float surface_depth = buffers.view_depth[index];
        if (ray_depth > surface_depth && ray_depth - surface_depth < settings.thickness) {
            ScreenHit hit;
            hit.hit = true;
            hit.radiance = buffers.radiance[index];
            if (settings.edge_fade > 0.0f) {
                const float fade_x = std::clamp((1.0f - std::fabs(ndc.x)) / settings.edge_fade, 0.0f, 1.0f);
                const float fade_y = std::clamp((1.0f - std::fabs(ndc.y)) / settings.edge_fade, 0.0f, 1.0f);
                hit.confidence = fade_x * fade_y;
            } else {
                hit.confidence = 1.0f;
            }
            return hit;
        }
    }
    return miss;
}

glm::vec3 reflection_radiance(const ScreenHit& hit, const glm::vec3& environment) noexcept {
    if (!hit.hit) {
        return environment;
    }
    return glm::mix(environment, hit.radiance, hit.confidence);
}

}  // namespace engine::gi
