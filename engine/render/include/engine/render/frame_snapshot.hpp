#pragma once

// Render extraction (US-033). extract_frame() runs at the synchronization point: it reads the scene graph, culls, and copies everything
// the GPU or the reference rasterizer needs into a FrameSnapshot. The snapshot holds plain values and entity handles, never pointers into
// the graph or the ECS registry, so it stays valid after the scene changes and can be handed to a render thread.

#include "engine/render/camera.hpp"
#include "engine/render/lighting.hpp"
#include "engine/render/mesh.hpp"

#include "engine/ecs/entity.hpp"
#include "engine/scene/scene_graph.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::render {

// The pose of one skinned instance (US-036): per joint, the object-space skinning matrix (the joint's model matrix times its inverse bind
// matrix). A vertex is skinned by the weighted sum of these. `previous` is the palette one frame earlier, for motion vectors; empty means
// the palette did not move.
struct SkinPose {
    std::vector<glm::mat4> palette;
    std::vector<glm::mat4> previous;
};

// One renderable as the author describes it: a scene node that draws a mesh with a flat color.
struct RenderableRef {
    ecs::EntityHandle node;
    std::uint32_t mesh = 0;      // index into the mesh bounds passed to extraction
    glm::vec4 color{1.0f};       // multiplies the material's base color
    std::uint32_t material = 0;  // index into the renderer's material table; 0 is the default material
    // The pose of a skinned mesh. Null draws the bind pose. Extraction copies the palette, so the pose need only live through the call.
    const SkinPose* skin = nullptr;
    // The node's world matrix one frame earlier. Empty when the node did not move, so its motion is zero.
    std::optional<glm::dmat4> previous_world;
};

// One object that survived culling. The model matrix is relative to the camera. An object outside the view can still cast a shadow into it,
// so extraction keeps those as shadow-only items: pick_id is zero, they are drawn into shadow maps and never into the scene.
struct SnapshotItem {
    std::uint32_t pick_id = 0;  // 1-based; the value an entity-ID target holds for this object. Zero means shadow-only (no pick, not in view).
    std::uint32_t mesh = 0;
    glm::mat4 model{1.0f};
    glm::vec4 color{1.0f};
    std::uint32_t material = 0;
    // The model matrix one frame earlier, relative to this frame's camera. Equal to `model` when the node did not move.
    glm::mat4 previous_model{1.0f};
    // Skinned items: the first entry of the palette in FrameSnapshot::palettes(), and how many entries. A zero count draws the bind pose.
    std::uint32_t palette = 0;
    std::uint32_t bone_count = 0;
};

// What the previous frame looked like from the camera, for motion vectors (US-036). The view-projection is the previous frame's, relative to
// the previous camera, and the position is the previous camera's world position. A frame with no history uses its own values, so its
// motion is zero.
struct MotionHistory {
    glm::mat4 view_projection{1.0f};
    glm::dvec3 camera_position{0.0};
};

struct ExtractionStats {
    std::uint32_t considered = 0;  // renderables passed in
    std::uint32_t stale = 0;       // the node was destroyed or never existed
    std::uint32_t unknown_mesh = 0;  // the mesh index is not in the bounds table
    std::uint32_t culled = 0;      // outside the frustum and out of every shadow volume
    std::uint32_t drawn = 0;       // visible: in the view, with a pick id
    std::uint32_t shadow_only = 0;  // outside the view but able to cast into it
};

class FrameSnapshot {
public:
    [[nodiscard]] const glm::mat4& view_projection() const noexcept {
        return view_projection_;
    }
    [[nodiscard]] const std::vector<SnapshotItem>& items() const noexcept {
        return items_;
    }
    [[nodiscard]] const ExtractionStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const Camera& camera() const noexcept {
        return camera_;
    }
    // The lights as the author gave them (world space), in the order they were passed in.
    [[nodiscard]] const std::vector<Light>& lights() const noexcept {
        return lights_;
    }
    // The palettes of the skinned items, concatenated: current, and one frame earlier (same indices).
    [[nodiscard]] const std::vector<glm::mat4>& palettes() const noexcept {
        return palettes_;
    }
    [[nodiscard]] const std::vector<glm::mat4>& previous_palettes() const noexcept {
        return previous_palettes_;
    }
    // The entity behind a pick id. Empty for zero, for an id outside the snapshot, and for an invalid handle.
    [[nodiscard]] std::optional<ecs::EntityHandle> entity_of(std::uint32_t pick_id) const noexcept;

private:
    friend FrameSnapshot extract_frame(scene::SceneGraph& graph, const Camera& camera, const std::vector<MeshBounds>& bounds,
                                       const std::vector<RenderableRef>& renderables, const std::vector<Light>& lights,
                                       const CascadeSettings& cascades, bool keep_offscreen);

    glm::mat4 view_projection_{1.0f};
    Camera camera_;
    std::vector<Light> lights_;
    std::vector<SnapshotItem> items_;
    std::vector<ecs::EntityHandle> entities_;  // entities_[pick_id - 1]
    std::vector<glm::mat4> palettes_;
    std::vector<glm::mat4> previous_palettes_;
    ExtractionStats stats_;
};

// Builds the snapshot for one frame. Dirty transforms are refreshed through the graph. A renderable whose node is not alive is skipped
// and counted as stale; one whose mesh index is outside `bounds` is skipped and counted as unknown_mesh. The camera must be valid
// (Camera::is_valid).
//
// When a directional light casts shadows, an object outside the view is kept as a shadow-only item if it lies in the cylinder that runs
// from the shadow volume toward the light (the camera frustum cut at cascades.shadow_distance). Its shadow reaches the view, so dropping
// it would make the shadow depend on what the camera sees.
//
// `keep_offscreen` keeps every other object outside the view as a shadow-only item too. The ray-traced pass (US-043) traces the snapshot's
// items, and a mirror in view can show an object that is off screen, so the caller that enables ray-traced reflections keeps them all. The
// scene draw never draws a shadow-only item, so the raster image is unchanged.
[[nodiscard]] FrameSnapshot extract_frame(scene::SceneGraph& graph, const Camera& camera, const std::vector<MeshBounds>& bounds,
                                          const std::vector<RenderableRef>& renderables, const std::vector<Light>& lights = {},
                                          const CascadeSettings& cascades = {}, bool keep_offscreen = false);

}  // namespace engine::render
