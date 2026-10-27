#include "engine/physics/collision_cells.hpp"

#include <cmath>
#include <limits>
#include <utility>

namespace engine::physics {

namespace {

bool finite(const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

CollisionCells::CollisionCells(PhysicsWorld& world, double unload_margin) : world_(&world), margin_(unload_margin) {}

core::Result<std::size_t, PhysicsError> CollisionCells::load(const CollisionCellDesc& desc) {
    if (cells_.contains(desc.id) || !finite(desc.min) || !finite(desc.max) || desc.min.x > desc.max.x || desc.min.z > desc.max.z ||
        !std::isfinite(margin_) || margin_ < 0.0) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    Cell cell;
    cell.min = desc.min;
    cell.max = desc.max;
    for (const BodyDesc& body : desc.bodies) {
        auto added = world_->add_body(body);
        if (!added.ok()) {
            const PhysicsError error = added.error();
            remove_bodies(cell);
            return core::failure(error);
        }
        cell.bodies.push_back(std::move(added).value());
    }
    const std::size_t count = cell.bodies.size();
    cells_.emplace(desc.id, std::move(cell));
    return core::success(count);
}

bool CollisionCells::unload(std::uint64_t id) {
    const auto it = cells_.find(id);
    if (it == cells_.end()) {
        return false;
    }
    if (column_clear(it->second)) {
        remove_bodies(it->second);
        cells_.erase(it);
    } else {
        it->second.unload_requested = true;
    }
    return true;
}

bool CollisionCells::cancel_unload(std::uint64_t id) {
    const auto it = cells_.find(id);
    if (it == cells_.end() || !it->second.unload_requested) {
        return false;
    }
    it->second.unload_requested = false;
    return true;
}

void CollisionCells::update() {
    for (auto it = cells_.begin(); it != cells_.end();) {
        if (it->second.unload_requested && column_clear(it->second)) {
            remove_bodies(it->second);
            it = cells_.erase(it);
        } else {
            ++it;
        }
    }
}

bool CollisionCells::loaded(std::uint64_t id) const {
    return cells_.contains(id);
}

bool CollisionCells::unload_pending(std::uint64_t id) const {
    const auto it = cells_.find(id);
    return it != cells_.end() && it->second.unload_requested;
}

std::size_t CollisionCells::cell_count() const {
    return cells_.size();
}

void CollisionCells::remove_bodies(Cell& cell) {
    for (const ecs::EntityHandle& entity : cell.bodies) {
        world_->remove_body(entity);
    }
    cell.bodies.clear();
}

// The column is the footprint grown by the margin, with no height limit: a body above the cell that is falling toward it keeps the cell.
bool CollisionCells::column_clear(const Cell& cell) const {
    constexpr double inf = std::numeric_limits<double>::infinity();
    const glm::dvec3 min(cell.min.x - margin_, -inf, cell.min.z - margin_);
    const glm::dvec3 max(cell.max.x + margin_, inf, cell.max.z + margin_);
    return !world_->has_moving_body_in(min, max);
}

core::Result<CollisionCellDesc, PhysicsError> terrain_tile_cell(const world::Terrain& terrain, world::TileCoord tile, std::uint64_t id,
                                                               ecs::EntityHandle entity) {
    const world::TerrainDesc& desc = terrain.desc;
    const std::size_t sample_count = static_cast<std::size_t>(terrain.samples_x) * terrain.samples_z;
    if (tile.x >= desc.tiles_x || tile.z >= desc.tiles_z || terrain.heights.size() != sample_count) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    const std::uint32_t side = desc.tile_cells + 1;
    if (side > kMaxHeightfieldSize) {
        return core::failure(PhysicsError::InvalidDesc);
    }

    const double span = static_cast<double>(desc.tile_cells) * static_cast<double>(desc.cell_size);
    const glm::dvec3 centre(static_cast<double>(tile.x) * span + 0.5 * span, 0.0, static_cast<double>(tile.z) * span + 0.5 * span);

    BodyDesc body;
    body.entity = entity;
    body.motion = BodyMotion::Static;
    body.shape = ShapeKind::Heightfield;
    body.grid_size = side;
    body.grid_spacing = desc.cell_size;
    body.position = centre;
    body.heights.reserve(static_cast<std::size_t>(side) * side);
    const std::uint32_t first_x = tile.x * desc.tile_cells;
    const std::uint32_t first_z = tile.z * desc.tile_cells;
    for (std::uint32_t iz = 0; iz < side; ++iz) {
        for (std::uint32_t ix = 0; ix < side; ++ix) {
            body.heights.push_back(world::sample_height(terrain, first_x + ix, first_z + iz));
        }
    }

    CollisionCellDesc cell;
    cell.id = id;
    cell.min = glm::dvec3(centre.x - 0.5 * span, 0.0, centre.z - 0.5 * span);
    cell.max = glm::dvec3(centre.x + 0.5 * span, 0.0, centre.z + 0.5 * span);
    cell.bodies.push_back(std::move(body));
    return core::success(std::move(cell));
}

CollisionCellStreamer::CollisionCellStreamer(CollisionCells& cells, const world::WorldDesc& world_desc, BodyBuilder build)
    : cells_(&cells), world_desc_(world_desc), build_(std::move(build)) {}

void CollisionCellStreamer::on_activated(const world::CellData& cell) {
    const std::uint64_t id = cell_key(cell->coord);
    if (cells_->loaded(id)) {
        // The cell was waiting to unload; it is active again, so its bodies stay.
        cells_->cancel_unload(id);
        return;
    }
    const world::CellBounds bounds = world::cell_bounds(world_desc_, cell->coord);
    CollisionCellDesc desc;
    desc.id = id;
    desc.min = glm::dvec3(static_cast<double>(bounds.min.x), 0.0, static_cast<double>(bounds.min.y));
    desc.max = glm::dvec3(static_cast<double>(bounds.max.x), 0.0, static_cast<double>(bounds.max.y));
    desc.bodies = build_(*cell);
    if (!cells_->load(desc).ok()) {
        ++failed_loads_;
    }
}

void CollisionCellStreamer::on_deactivated(const world::CellData& cell) {
    cells_->unload(cell_key(cell->coord));
}

std::size_t CollisionCellStreamer::failed_loads() const noexcept {
    return failed_loads_;
}

std::uint64_t CollisionCellStreamer::cell_key(world::CellCoord coord) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.x)) << 32) | static_cast<std::uint32_t>(coord.z);
}

core::Result<ecs::EntityHandle, PhysicsError> refresh_terrain_collider(PhysicsWorld& physics, const world::Terrain& terrain,
                                                                      world::TileCoord tile, ecs::EntityHandle entity) {
    // replace_collider keeps the body and its cell, so the cell id passed to the description is not used.
    auto cell = terrain_tile_cell(terrain, tile, 0, entity);
    if (!cell.ok()) {
        return core::failure(cell.error());
    }
    return physics.replace_collider(entity, cell.value().bodies.front());
}

}  // namespace engine::physics
