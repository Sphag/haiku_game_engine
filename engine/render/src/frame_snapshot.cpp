#include "engine/render/frame_snapshot.hpp"

#include <algorithm>
#include <cmath>

namespace engine::render {

std::optional<ecs::EntityHandle> FrameSnapshot::entity_of(std::uint32_t pick_id) const noexcept {
    if (pick_id == 0 || pick_id > entities_.size()) {
        return std::nullopt;
    }
    return entities_[pick_id - 1];
}

namespace {

// A sphere that encloses the camera's frustum from the eye to the shadow distance. The apex is on its surface, so its center lies on the
// view axis at the distance that minimizes the radius.
struct ShadowVolume {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    bool valid = false;
};

ShadowVolume shadow_volume(const Camera& camera, float shadow_distance) {
    ShadowVolume volume;
    const double distance = std::min(camera.far_plane, static_cast<double>(shadow_distance));
    if (!(distance > 0.0)) {
        return volume;
    }
    const double half_h = std::tan(camera.fov_y_radians * 0.5) * distance;
    const double half_w = half_h * camera.aspect;
    const double k = half_w * half_w + half_h * half_h;
    const double t = (distance * distance + k) / (2.0 * distance);
    volume.center = glm::vec3(glm::vec3(camera.forward()) * static_cast<float>(t));
    volume.radius = static_cast<float>(t);
    volume.valid = true;
    return volume;
}

// True when a sphere is on the light's side of the volume and close enough to its axis to shadow it. The test is conservative: a caster
// that is kept but cannot reach the view costs only a shadow draw.
bool shadows_volume(const ShadowVolume& volume, const glm::vec3& light_direction, const glm::vec3& center, float radius) {
    const glm::vec3 offset = center - volume.center;
    const glm::vec3 toward_light = -light_direction;
    const float along = glm::dot(offset, toward_light);
    const glm::vec3 lateral = offset - along * toward_light;
    return glm::length(lateral) <= volume.radius + radius && along >= -volume.radius - radius;
}

}  // namespace

FrameSnapshot extract_frame(scene::SceneGraph& graph, const Camera& camera, const std::vector<MeshBounds>& bounds,
                            const std::vector<RenderableRef>& renderables, const std::vector<Light>& lights, const CascadeSettings& cascades,
                            bool keep_offscreen) {
    FrameSnapshot snapshot;
    snapshot.view_projection_ = camera_view_projection(camera);
    snapshot.camera_ = camera;
    snapshot.lights_ = lights;
    const Frustum frustum = make_frustum(snapshot.view_projection_);
    const glm::dvec3 eye = camera.position;

    // Directional shadows reach the view from outside it, so their casters are kept as well.
    ShadowVolume volume;
    std::vector<glm::vec3> shadow_directions;
    for (const Light& light : lights) {
        if (light.casts_shadow && light.type == LightType::Directional) {
            shadow_directions.push_back(light.direction);
        }
    }
    if (!shadow_directions.empty()) {
        volume = shadow_volume(camera, cascades.shadow_distance);
    }

    snapshot.stats_.considered = static_cast<std::uint32_t>(renderables.size());
    for (const RenderableRef& renderable : renderables) {
        const glm::dmat4* world = graph.world_matrix(renderable.node);
        if (world == nullptr) {
            ++snapshot.stats_.stale;
            continue;
        }
        if (renderable.mesh >= bounds.size()) {
            ++snapshot.stats_.unknown_mesh;
            continue;
        }
        const MeshBounds& mesh_bounds = bounds[renderable.mesh];

        // Camera-relative placement in double, then one conversion to float. The bounding sphere uses the same placement.
        glm::dmat4 relative = *world;
        relative[3] = glm::dvec4(glm::dvec3(relative[3]) - eye, 1.0);
        const double scale = std::max({glm::length(glm::dvec3(relative[0])), glm::length(glm::dvec3(relative[1])),
                                       glm::length(glm::dvec3(relative[2]))});
        const glm::dvec3 center = glm::dvec3(relative * glm::dvec4(glm::dvec3(mesh_bounds.center), 1.0));
        const float radius = static_cast<float>(mesh_bounds.radius * scale);
        const bool visible = sphere_visible(frustum, glm::vec3(center), radius);
        bool shadow_reach = false;
        if (!visible && volume.valid) {
            for (const glm::vec3& direction : shadow_directions) {
                if (shadows_volume(volume, direction, glm::vec3(center), radius)) {
                    shadow_reach = true;
                    break;
                }
            }
        }
        if (!visible && !shadow_reach && !keep_offscreen) {
            ++snapshot.stats_.culled;
            continue;
        }

        SnapshotItem item;
        if (visible) {
            snapshot.entities_.push_back(renderable.node);
            item.pick_id = static_cast<std::uint32_t>(snapshot.entities_.size());
        } else {
            ++snapshot.stats_.shadow_only;
        }
        item.mesh = renderable.mesh;
        item.model = glm::mat4(relative);
        item.color = renderable.color;
        item.material = renderable.material;
        item.previous_model = item.model;
        if (renderable.previous_world) {
            glm::dmat4 previous = *renderable.previous_world;
            previous[3] = glm::dvec4(glm::dvec3(previous[3]) - eye, 1.0);
            item.previous_model = glm::mat4(previous);
        }
        if (renderable.skin != nullptr && !renderable.skin->palette.empty()) {
            const std::vector<glm::mat4>& palette = renderable.skin->palette;
            const std::vector<glm::mat4>& previous = renderable.skin->previous.size() == palette.size() ? renderable.skin->previous : palette;
            item.palette = static_cast<std::uint32_t>(snapshot.palettes_.size());
            item.bone_count = static_cast<std::uint32_t>(palette.size());
            snapshot.palettes_.insert(snapshot.palettes_.end(), palette.begin(), palette.end());
            snapshot.previous_palettes_.insert(snapshot.previous_palettes_.end(), previous.begin(), previous.end());
        }
        snapshot.items_.push_back(item);
    }
    snapshot.stats_.drawn = static_cast<std::uint32_t>(snapshot.items_.size()) - snapshot.stats_.shadow_only;
    return snapshot;
}

}  // namespace engine::render
