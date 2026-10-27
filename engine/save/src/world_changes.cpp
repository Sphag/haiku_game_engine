#include "engine/save/world_changes.hpp"

#include "engine/core/file_io.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace engine::save {
namespace {

SaveError invalid_content(std::string message) {
    return SaveError{SaveErrorCode::InvalidContent, std::move(message)};
}

}  // namespace

WorldChanges collect_world_changes(const world::World& world) {
    WorldChanges changes;
    for (const auto& cell : world.cells) {
        if (cell.dirty) {
            changes.cells.push_back(cell);
        }
    }
    if (world.layer.dirty) {
        changes.layer = world.layer;
    }
    return changes;
}

core::Result<core::Unit, SaveError> apply_world_changes(world::World& world, const WorldChanges& changes) {
    // Work on a copy, so the world is changed only when every cell and the whole result pass their checks.
    world::World next = world;
    for (const auto& saved : changes.cells) {
        std::optional<std::size_t> same_id;
        std::optional<std::size_t> same_coord;
        for (std::size_t i = 0; i < next.cells.size(); ++i) {
            if (next.cells[i].id == saved.id) {
                same_id = i;
            }
            if (next.cells[i].coord == saved.coord) {
                same_coord = i;
            }
        }

        world::WorldCell cell = saved;
        cell.dirty = true;
        if (same_id) {
            if (next.cells[*same_id].coord != saved.coord) {
                return core::failure(invalid_content("cell " + saved.id.to_string() + " moved to another coordinate"));
            }
            next.cells[*same_id] = std::move(cell);
        } else if (same_coord) {
            return core::failure(invalid_content("a saved cell at a coordinate that another cell holds in the authored world"));
        } else {
            next.cells.push_back(std::move(cell));
        }
    }
    if (changes.layer) {
        next.layer = *changes.layer;
        next.layer.dirty = true;
    }

    const auto problems = world::validate_world(next);
    if (!problems.empty()) {
        return core::failure(invalid_content("world: " + problems.front()));
    }
    world = std::move(next);
    return core::success();
}

core::Result<core::Unit, SaveError> write_cell_files(const core::Path& directory, const WorldChanges& changes) {
    if (auto created = core::create_directories(directory); !created.ok()) {
        return core::failure(SaveError{SaveErrorCode::Io, core::describe(created.error())});
    }
    for (const auto& cell : changes.cells) {
        auto path = directory.join(world::cell_file_name(cell));
        if (!path.ok()) {
            return core::failure(invalid_content("a cell file name is not a valid path"));
        }
        if (auto written = core::write_file_atomic(path.value(), world::serialize_cell(cell)); !written.ok()) {
            return core::failure(SaveError{SaveErrorCode::Io, core::describe(written.error())});
        }
    }
    if (changes.layer) {
        auto path = directory.join(std::string(world::kLayerFileName));
        if (!path.ok()) {
            return core::failure(invalid_content("the layer file name is not a valid path"));
        }
        if (auto written = core::write_file_atomic(path.value(), world::serialize_layer(*changes.layer)); !written.ok()) {
            return core::failure(SaveError{SaveErrorCode::Io, core::describe(written.error())});
        }
    }
    return core::success();
}

}  // namespace engine::save
