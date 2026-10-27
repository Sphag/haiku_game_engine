#pragma once

// World partition data (US-048): the authored description of a large world, on the CPU. The world is a uniform grid of cells on the XZ
// plane, plus one always-loaded layer that belongs to no cell. Every cell has a stable GUID, bounds, the assets its objects depend on,
// and its own objects. Ownership is half-open: a point at x = k * cell_size belongs to the cell whose index is k, so an object on a
// boundary has exactly one owner. Objects keep their GUIDs when they move, and a reference names its target by GUID, so it resolves
// across cells and into the always-loaded layer.
//
// Cells are created on demand and never removed, so a cell GUID stays valid for the life of the world. Edits mark the cell they change
// as dirty; save_dirty writes only those cells. The text formats carry a schema version and reject any other version, unknown keywords,
// and truncated files. Terrain tiles are assigned to cells by the converter (converter/world_partition), not stored here.

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::world {

inline constexpr int kWorldFormatVersion = 1;
inline constexpr std::string_view kCellFormat = "engine.world_cell";
inline constexpr std::string_view kLayerFormat = "engine.world_layer";
inline constexpr float kMaxCellSize = 65536.0f;

// Cell grid placement. The owner of a point is floor((xz - origin) / cell_size) on each axis.
struct WorldDesc {
    glm::vec2 origin{0.0f};
    float cell_size = 256.0f;  // Metres, positive and finite, at most kMaxCellSize.
};

struct CellCoord {
    std::int32_t x = 0;
    std::int32_t z = 0;

    friend bool operator==(const CellCoord& a, const CellCoord& b) noexcept {
        return a.x == b.x && a.z == b.z;
    }
    friend bool operator!=(const CellCoord& a, const CellCoord& b) noexcept {
        return !(a == b);
    }
};

// Half-open XZ rectangle [min, max) of a cell in world metres.
struct CellBounds {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
};

// One placed object. `asset` names the asset it instantiates; `references` name other objects by GUID, in any cell or in the layer.
struct WorldObject {
    core::Guid id;
    std::string name;  // One line; it may be empty and may contain spaces.
    core::Guid asset;
    glm::vec3 position{0.0f};
    std::vector<core::Guid> references;
};

struct WorldCell {
    core::Guid id;
    CellCoord coord;
    std::vector<core::Guid> assets;  // Asset dependencies: each object's asset, added when the object enters the cell.
    std::vector<WorldObject> objects;
    bool dirty = false;
};

// Objects that stay loaded whatever the camera does. They have no cell and no bounds, and they are saved as one file.
struct AlwaysLoadedLayer {
    std::vector<WorldObject> objects;
    bool dirty = false;
};

struct World {
    WorldDesc desc;
    std::vector<WorldCell> cells;
    AlwaysLoadedLayer layer;
};

// Where an object lives: the layer, or the cell at `cell` and the object at `index` within it.
struct ObjectLocation {
    bool in_layer = false;
    std::size_t cell = 0;
    std::size_t index = 0;
};

[[nodiscard]] core::Result<core::Unit, std::string> validate_world_desc(const WorldDesc& desc);
[[nodiscard]] core::Result<World, std::string> create_world(const WorldDesc& desc);

// The cell that owns a point. Boundaries are half-open, so this is a pure function of the point and the grid.
[[nodiscard]] CellCoord cell_coord_at(const WorldDesc& desc, glm::dvec2 xz) noexcept;
[[nodiscard]] CellBounds cell_bounds(const WorldDesc& desc, CellCoord coord) noexcept;

// Returns the index of the cell at `coord`, creating it with a new GUID and dirty flag when it does not exist yet.
[[nodiscard]] std::size_t ensure_cell(World& world, CellCoord coord);

// Adds an object to the layer (`always_loaded`) or to the cell that owns its position. Errors on a nil GUID or asset, a duplicate
// GUID anywhere in the world, a multi-line name, or a position that is not finite or lies outside the grid's index range.
[[nodiscard]] core::Result<core::Unit, std::string> add_object(World& world, WorldObject object, bool always_loaded);

[[nodiscard]] std::optional<ObjectLocation> find_object(const World& world, const core::Guid& id);
[[nodiscard]] const WorldObject& object_at(const World& world, const ObjectLocation& location);

// Moves an object to a new position. A position in another cell moves the object into that cell and out of the old one; its GUID,
// name, and references are kept. An always-loaded object changes position only, since it has no cell.
[[nodiscard]] core::Result<core::Unit, std::string> move_object(World& world, const core::Guid& id, glm::vec3 position);

// Checks the whole world and returns one message per problem; an empty list means the world is valid. It checks unique GUIDs, unique
// cell coordinates, the single owner of every object, that each cell lists the assets its objects use, and that every reference
// resolves to an object in some cell or in the layer.
[[nodiscard]] std::vector<std::string> validate_world(const World& world);

[[nodiscard]] std::string serialize_cell(const WorldCell& cell);
[[nodiscard]] core::Result<WorldCell, std::string> parse_cell(std::string_view text);
[[nodiscard]] std::string serialize_layer(const AlwaysLoadedLayer& layer);
[[nodiscard]] core::Result<AlwaysLoadedLayer, std::string> parse_layer(std::string_view text);

// File names inside a world directory: "<cell guid>.cell" and "layer.world".
[[nodiscard]] std::string cell_file_name(const WorldCell& cell);
inline constexpr std::string_view kLayerFileName = "layer.world";

// Writes each dirty cell and a dirty layer into `directory`, creating it if needed. Each file is written atomically, and its dirty flag
// is cleared only after the write succeeds. Returns the number of files written.
[[nodiscard]] core::Result<std::size_t, std::string> save_dirty(World& world, const core::Path& directory);

[[nodiscard]] core::Result<WorldCell, std::string> load_cell(const core::Path& file);
[[nodiscard]] core::Result<AlwaysLoadedLayer, std::string> load_layer(const core::Path& file);

}  // namespace engine::world
