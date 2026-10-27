#pragma once

// World changes in a save (US-059): the cells and the always-loaded layer whose dirty flag is set. A game save only reads the flags, so
// the authored world on disk is never rewritten by saving. Applying a save marks the restored cells dirty again.
//
// Restoring unloaded cells: apply_world_changes puts the saved cells into the in-memory World. A streaming manager reads cell files
// from a directory, so write_cell_files writes the restored cells into a runtime directory the manager is given. That directory must
// not be the authored project directory, or a load would overwrite the authored cells.

#include "engine/save/save_error.hpp"

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/world/partition.hpp"

#include <optional>
#include <vector>

namespace engine::save {

struct WorldChanges {
    std::vector<world::WorldCell> cells;             // cells whose dirty flag was set, in the world's order
    std::optional<world::AlwaysLoadedLayer> layer;   // present when the always-loaded layer is dirty
};

// Copies the dirty cells and layer. The world is not changed, so its dirty flags stay set.
[[nodiscard]] WorldChanges collect_world_changes(const world::World& world);

// Applies saved changes. A saved cell replaces the cell with the same GUID, which must be at the same coordinate. A new GUID at a
// coordinate that another cell holds is a conflict. The whole result is validated (world::validate_world) before the world changes,
// so a failure leaves the world as it was. Every applied cell and the layer are marked dirty.
[[nodiscard]] core::Result<core::Unit, SaveError> apply_world_changes(world::World& world, const WorldChanges& changes);

// Writes each changed cell as "<cell guid>.cell" and the layer as "layer.world" in `directory`, each atomically. The directory is
// created when missing.
[[nodiscard]] core::Result<core::Unit, SaveError> write_cell_files(const core::Path& directory, const WorldChanges& changes);

}  // namespace engine::save
