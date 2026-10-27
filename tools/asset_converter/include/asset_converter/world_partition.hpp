#pragma once

// World preparation (US-048): the converter turns an authored world and its terrain into the files a runtime reads. For every cell it
// writes the cell file, a manifest entry, and one blob of distant terrain LODs. A terrain tile belongs to the cell that owns its origin
// corner, using the same half-open rule as objects, so each tile is prepared exactly once. Distant terrain uses one uniform LOD, clamped
// to the terrain's maximum. The preparation is pure: prepare_world builds the bytes, and publish_world writes them.
//
// Output layout in the directory:
//   <cell guid>.cell          the authored cell (engine.world_cell)
//   <cell guid>.terrain_lod   the cell's distant terrain tiles (magic "OWTL"), written only when the cell owns a tile
//   layer.world               the always-loaded layer (engine.world_layer), written even when it is empty
//   manifest.txt              the grid, one line for the layer, and one line per cell, each with a content hash (engine.world_manifest)

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/terrain.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::converter {

inline constexpr std::string_view kWorldManifestFormat = "engine.world_manifest";
inline constexpr int kWorldManifestVersion = 1;
inline constexpr std::string_view kTerrainLodMagic = "OWTL";
inline constexpr std::uint32_t kTerrainLodBlobVersion = 1;
inline constexpr std::string_view kWorldManifestName = "manifest.txt";

struct WorldPrepareOptions {
    std::uint32_t distant_lod = 2;  // Clamped to the terrain's maximum LOD.
};

struct PreparedCell {
    core::Guid id;
    world::CellCoord coord;
    std::size_t object_count = 0;
    std::size_t terrain_tiles = 0;
    std::uint32_t terrain_lod = 0;  // The distant LOD of the cell's tiles; zero when it owns none.
    std::string cell_text;          // Contents of <guid>.cell
    std::string terrain_blob;       // Contents of <guid>.terrain_lod; empty when the cell owns no tile.
    std::string hash;               // 16 hex digits over the cell text and the blob.
};

struct PreparedWorld {
    std::string layer_text;     // Contents of layer.world
    std::size_t layer_objects = 0;
    std::string layer_hash;
    std::vector<PreparedCell> cells;  // Sorted by cell GUID, so the output does not depend on creation order.
    std::string manifest;             // Contents of manifest.txt
};

// Builds every prepared file. `terrain` may be null for a world without terrain. Errors when a terrain tile's owning cell does not
// exist, since the tile would otherwise have no place to be stored.
[[nodiscard]] core::Result<PreparedWorld, std::string> prepare_world(const world::World& world, const world::Terrain* terrain,
                                                                     const WorldPrepareOptions& options);

// Writes the prepared files into `directory`, creating it if needed. Each file is written atomically.
[[nodiscard]] core::Result<core::Unit, std::string> publish_world(const PreparedWorld& prepared, const core::Path& directory);

}  // namespace engine::converter
