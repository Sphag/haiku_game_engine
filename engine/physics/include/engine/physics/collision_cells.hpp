#pragma once

// Streamed collision cells (US-053). A cell owns the colliders of one streamed terrain or scene region. Loading a cell adds its bodies to
// the world, and unloading removes them. An unload is deferred while a dynamic or kinematic body is in the cell's column (its footprint
// grown by a margin, at any height), so a body falling toward the cell or resting on it keeps the colliders it needs. The deferred
// unload completes in update() once the column is clear.

#include "engine/core/result.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"
#include "engine/world/terrain.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace engine::physics {

inline constexpr double kDefaultCellUnloadMargin = 4.0;  // meters the column grows beyond the cell's footprint

struct CollisionCellDesc {
    std::uint64_t id = 0;
    glm::dvec3 min{0.0};  // world meters: the cell's footprint in x and z
    glm::dvec3 max{0.0};
    std::vector<BodyDesc> bodies;  // each body's entity must be unique in the world
};

class CollisionCells {
public:
    // The world is not owned and must outlive the cells. Once the world closes, its bodies are gone and unloads only forget the cells.
    explicit CollisionCells(PhysicsWorld& world, double unload_margin = kDefaultCellUnloadMargin);

    CollisionCells(const CollisionCells&) = delete;
    CollisionCells& operator=(const CollisionCells&) = delete;
    CollisionCells(CollisionCells&&) = delete;
    CollisionCells& operator=(CollisionCells&&) = delete;

    // Adds the cell's bodies and returns how many were added. A cell that is loaded (including one waiting to unload) is InvalidDesc.
    // If any body fails, the bodies already added are removed and the cell is not loaded.
    [[nodiscard]] core::Result<std::size_t, PhysicsError> load(const CollisionCellDesc& desc);

    // Requests the unload. The cell's bodies are removed now when its column is clear; otherwise the unload waits for update().
    // Returns false when the cell is not loaded.
    bool unload(std::uint64_t id);

    // Cancels a pending unload, so the cell keeps its bodies. Returns false when the cell is not loaded or no unload is pending.
    bool cancel_unload(std::uint64_t id);

    // Completes the deferred unloads whose columns are clear. Call once per frame, after the physics update.
    void update();

    // True while the cell is loaded, including a cell whose unload is waiting.
    [[nodiscard]] bool loaded(std::uint64_t id) const;
    [[nodiscard]] bool unload_pending(std::uint64_t id) const;
    [[nodiscard]] std::size_t cell_count() const;

private:
    struct Cell {
        glm::dvec3 min{0.0};
        glm::dvec3 max{0.0};
        std::vector<ecs::EntityHandle> bodies;
        bool unload_requested = false;
    };

    void remove_bodies(Cell& cell);
    [[nodiscard]] bool column_clear(const Cell& cell) const;

    PhysicsWorld* world_;
    double margin_;
    std::map<std::uint64_t, Cell> cells_;  // ordered, so updates run in a stable order
};

// The collision cell of one terrain tile (US-053): a static heightfield over the tile's (tile_cells + 1)^2 samples, centred on the tile in
// x and z, so the cell's footprint is the tile's footprint. The heights are absolute world heights. `entity` owns the body and must be
// unique in the world. Fails with InvalidDesc for a tile outside the terrain or a grid larger than kMaxHeightfieldSize.
[[nodiscard]] core::Result<CollisionCellDesc, PhysicsError> terrain_tile_cell(const world::Terrain& terrain, world::TileCoord tile,
                                                                               std::uint64_t id, ecs::EntityHandle entity);

// Rebuilds the heightfield of a terrain tile's body after an edit (US-071). `entity` owns the body already in the world, which keeps its
// id, pose, and cell membership; only the shape changes. Fails with InvalidEntity for a body the world does not hold, and with
// InvalidDesc for a tile outside the terrain. On failure the body keeps its old shape, so the caller can retry the tile.
[[nodiscard]] core::Result<ecs::EntityHandle, PhysicsError> refresh_terrain_collider(PhysicsWorld& physics, const world::Terrain& terrain,
                                                                                    world::TileCoord tile, ecs::EntityHandle entity);

// Keeps collision cells in step with the world streamer (US-053). A cell that activates loads the bodies that `build` returns for it, and
// a cell that deactivates requests its unload, which waits while a moving body is in its column. A cell that reactivates before its
// unload completes keeps its bodies. The cell id is the packed grid coordinate, and the footprint comes from the world's descriptor. The
// CollisionCells object is not owned and must outlive the subscriber.
class CollisionCellStreamer final : public world::CellSubscriber {
public:
    using BodyBuilder = std::function<std::vector<BodyDesc>(const world::WorldCell& cell)>;

    CollisionCellStreamer(CollisionCells& cells, const world::WorldDesc& world_desc, BodyBuilder build);

    void on_activated(const world::CellData& cell) override;
    void on_deactivated(const world::CellData& cell) override;

    // Activations whose bodies the world refused. Such a cell is not loaded, and its next activation tries again.
    [[nodiscard]] std::size_t failed_loads() const noexcept;

    [[nodiscard]] static std::uint64_t cell_key(world::CellCoord coord) noexcept;

private:
    CollisionCells* cells_;
    world::WorldDesc world_desc_;
    BodyBuilder build_;
    std::size_t failed_loads_ = 0;
};

}  // namespace engine::physics
