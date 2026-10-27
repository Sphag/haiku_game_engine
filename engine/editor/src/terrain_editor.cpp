#include "engine/editor/terrain_editor.hpp"

#include "engine/ecs/world.hpp"
#include "engine/physics/collision_cells.hpp"
#include "engine/physics/physics_world.hpp"

#include <cstdint>
#include <utility>

namespace engine::editor {

namespace {

const char* physics_error_name(physics::PhysicsError error) {
    switch (error) {
        case physics::PhysicsError::InvalidState:
            return "the physics world is closed";
        case physics::PhysicsError::InvalidEntity:
            return "the entity is invalid or already has a body";
        case physics::PhysicsError::InvalidDesc:
            return "the terrain collider description is invalid";
        case physics::PhysicsError::BodyLimit:
            return "the physics world has no free body slot";
    }
    return "unknown physics error";
}

}  // namespace

core::Result<TerrainEditor, std::string> TerrainEditor::create(const world::TerrainDesc& desc) {
    auto document = world::TerrainDocument::create(desc);
    if (!document.ok()) {
        return core::failure(std::string(document.error()));
    }
    return core::success(TerrainEditor(std::move(document.value())));
}

core::Result<TerrainEditor, std::string> TerrainEditor::open(const core::Path& path) {
    auto terrain = world::load_terrain(path);
    if (!terrain.ok()) {
        return core::failure(std::string(terrain.error()));
    }
    return core::success(TerrainEditor(world::TerrainDocument::from_terrain(std::move(terrain.value()))));
}

core::Result<core::Unit, std::string> TerrainEditor::save(const core::Path& path) {
    auto written = world::save_terrain(path, terrain());
    if (!written.ok()) {
        return core::failure(std::string(written.error()));
    }
    document_.mark_saved();
    return core::success(core::Unit{});
}

TerrainEditor::TerrainEditor(world::TerrainDocument document) : document_(std::move(document)) {
    const std::size_t count = static_cast<std::size_t>(terrain().desc.tiles_x) * terrain().desc.tiles_z;
    meshes_.resize(count);
    mesh_revision_.assign(count, 0);
    colliders_.assign(count, ecs::EntityHandle{});
    collider_revision_.assign(count, 0);
}

std::size_t TerrainEditor::tile_slot(world::TileCoord tile) const noexcept {
    return static_cast<std::size_t>(tile.z) * terrain().desc.tiles_x + tile.x;
}

core::Result<core::Unit, std::string> TerrainEditor::begin_stroke() {
    return document_.begin_stroke(brush_);
}

core::Result<core::Unit, std::string> TerrainEditor::stamp(glm::vec2 centre_world) {
    return document_.stamp(centre_world);
}

core::Result<bool, std::string> TerrainEditor::end_stroke() {
    return document_.end_stroke();
}

core::Result<core::Unit, std::string> TerrainEditor::cancel_stroke() {
    return document_.cancel_stroke();
}

bool TerrainEditor::undo() {
    return document_.undo();
}

bool TerrainEditor::redo() {
    return document_.redo();
}

core::Result<const world::TerrainTileMesh*, std::string> TerrainEditor::tile_mesh(world::TileCoord tile) {
    if (tile.x >= terrain().desc.tiles_x || tile.z >= terrain().desc.tiles_z) {
        return core::failure(std::string("tile is outside the terrain"));
    }
    const std::size_t slot = tile_slot(tile);
    const std::uint64_t revision = document_.tile_revision(tile);
    if (!meshes_[slot].has_value() || mesh_revision_[slot] != revision) {
        // Every tile renders at LOD 0 in the editor, so no neighbour is stitched and the edges read as LOD 0.
        auto built = world::build_tile_mesh(terrain(), tile, 0, {0, 0, 0, 0});
        if (!built.ok()) {
            return core::failure(std::string(built.error()));
        }
        meshes_[slot] = std::move(built.value());
        mesh_revision_[slot] = revision;
    }
    return core::success(static_cast<const world::TerrainTileMesh*>(&*meshes_[slot]));
}

core::Result<core::Unit, std::string> TerrainEditor::ensure_colliders(physics::PhysicsWorld& physics, ecs::World& ecs) {
    const world::TerrainDesc& desc = terrain().desc;
    for (std::uint32_t z = 0; z < desc.tiles_z; ++z) {
        for (std::uint32_t x = 0; x < desc.tiles_x; ++x) {
            const world::TileCoord tile{x, z};
            const std::size_t slot = tile_slot(tile);
            if (colliders_[slot].valid()) {
                continue;
            }
            const ecs::EntityHandle owner = ecs.create();
            // The cell id only has to be unique within this editor's physics world; the slot plus one is.
            auto cell = physics::terrain_tile_cell(terrain(), tile, static_cast<std::uint64_t>(slot) + 1, owner);
            if (!cell.ok()) {
                return core::failure(std::string("terrain collider for a tile: ") + physics_error_name(cell.error()));
            }
            auto body = physics.add_body(cell.value().bodies.front());
            if (!body.ok()) {
                return core::failure(std::string("terrain collider for a tile: ") + physics_error_name(body.error()));
            }
            colliders_[slot] = body.value();
            collider_revision_[slot] = document_.tile_revision(tile);
        }
    }
    return core::success(core::Unit{});
}

bool TerrainEditor::has_collider(world::TileCoord tile) const {
    if (tile.x >= terrain().desc.tiles_x || tile.z >= terrain().desc.tiles_z) {
        return false;
    }
    return colliders_[tile_slot(tile)].valid();
}

TerrainRefresh TerrainEditor::refresh(physics::PhysicsWorld* physics) {
    TerrainRefresh out;
    for (const world::TileCoord& tile : document_.take_dirty_tiles()) {
        meshes_[tile_slot(tile)].reset();
        out.invalidated.push_back(tile);
    }
    if (physics == nullptr) {
        return out;
    }
    // A body is stale when its tile's revision moved past the one it was last built at. That covers a failed earlier refresh and an
    // undo or redo that happened without a refresh.
    const world::TerrainDesc& desc = terrain().desc;
    for (std::uint32_t z = 0; z < desc.tiles_z; ++z) {
        for (std::uint32_t x = 0; x < desc.tiles_x; ++x) {
            const world::TileCoord tile{x, z};
            const std::size_t slot = tile_slot(tile);
            const std::uint64_t revision = document_.tile_revision(tile);
            if (!colliders_[slot].valid() || collider_revision_[slot] == revision) {
                continue;
            }
            auto refreshed = physics::refresh_terrain_collider(*physics, terrain(), tile, colliders_[slot]);
            if (refreshed.ok()) {
                collider_revision_[slot] = revision;
                out.colliders_refreshed.push_back(tile);
            } else {
                out.collider_failures.push_back(tile);
            }
        }
    }
    return out;
}

}  // namespace engine::editor
