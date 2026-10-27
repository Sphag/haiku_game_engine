#pragma once

// Terrain editing session (US-071) for the editor, with no ImGui or device dependency. It owns one TerrainDocument, the brush settings,
// and two per-tile caches that follow the document: the LOD mesh of each tile, which is built on demand, and the physics collider of each
// tile, which the session creates and refreshes. A refresh invalidates the caches of exactly the tiles the edits touched (the document's
// dirty tiles), so an untouched tile keeps its mesh and its body. A collider that fails to refresh keeps its old shape and is reported,
// so the caller can retry it.

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/world/terrain.hpp"
#include "engine/world/terrain_edit.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine::ecs {
class World;
}
namespace engine::physics {
class PhysicsWorld;
}

namespace engine::editor {

// The tiles that one refresh touched.
struct TerrainRefresh {
    std::vector<world::TileCoord> invalidated;          // Tiles whose LOD mesh was dropped; rebuilt on the next tile_mesh call.
    std::vector<world::TileCoord> colliders_refreshed;  // Tiles whose physics body took the new heightfield.
    std::vector<world::TileCoord> collider_failures;    // Tiles whose body could not be refreshed; the body keeps its old shape.
};

class TerrainEditor {
public:
    // A flat terrain of the given description. Fails for an invalid description.
    [[nodiscard]] static core::Result<TerrainEditor, std::string> create(const world::TerrainDesc& desc);
    // Opens a saved .terrain file. The document starts clean.
    [[nodiscard]] static core::Result<TerrainEditor, std::string> open(const core::Path& path);
    // Writes the terrain and marks the document clean. A failed write leaves the document dirty.
    [[nodiscard]] core::Result<core::Unit, std::string> save(const core::Path& path);

    [[nodiscard]] const world::TerrainDocument& document() const noexcept {
        return document_;
    }
    [[nodiscard]] const world::Terrain& terrain() const noexcept {
        return document_.terrain();
    }

    // The brush the next stroke uses. Changing it is not an edit; the stroke validates it when it begins.
    [[nodiscard]] world::Brush& brush() noexcept {
        return brush_;
    }
    [[nodiscard]] const world::Brush& brush() const noexcept {
        return brush_;
    }

    // Stroke commands. A stroke is one undo command; see TerrainDocument.
    [[nodiscard]] core::Result<core::Unit, std::string> begin_stroke();
    [[nodiscard]] core::Result<core::Unit, std::string> stamp(glm::vec2 centre_world);
    [[nodiscard]] core::Result<bool, std::string> end_stroke();
    [[nodiscard]] core::Result<core::Unit, std::string> cancel_stroke();
    bool undo();
    bool redo();

    // The LOD-0 mesh of a tile, built when it is first asked for or after a refresh invalidated it. Fails for a tile outside the terrain.
    // The pointer stays valid until the next refresh or stroke command.
    [[nodiscard]] core::Result<const world::TerrainTileMesh*, std::string> tile_mesh(world::TileCoord tile);

    // Creates a body for every tile that lacks one, in `physics`, each owned by a new entity of `ecs`. Tiles that already have a body are
    // skipped. Fails when a body cannot be added; the bodies added before the failure stay.
    [[nodiscard]] core::Result<core::Unit, std::string> ensure_colliders(physics::PhysicsWorld& physics, ecs::World& ecs);
    // The body of a tile, when ensure_colliders created it.
    [[nodiscard]] bool has_collider(world::TileCoord tile) const;

    // Takes the document's dirty tiles, drops their LOD meshes, and refreshes their bodies when `physics` is given. Call after a stroke,
    // an undo, or a redo.
    [[nodiscard]] TerrainRefresh refresh(physics::PhysicsWorld* physics);

private:
    explicit TerrainEditor(world::TerrainDocument document);

    [[nodiscard]] std::size_t tile_slot(world::TileCoord tile) const noexcept;

    world::TerrainDocument document_;
    world::Brush brush_;
    std::vector<std::optional<world::TerrainTileMesh>> meshes_;  // One per tile, row-major in Z, then X.
    std::vector<std::uint64_t> mesh_revision_;                    // The document revision each mesh was built at.
    std::vector<ecs::EntityHandle> colliders_;                    // One per tile; invalid when the tile has no body.
    std::vector<std::uint64_t> collider_revision_;                // The document revision each body was built at.
};

}  // namespace engine::editor
