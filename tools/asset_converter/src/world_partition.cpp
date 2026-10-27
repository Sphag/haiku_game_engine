#include "asset_converter/world_partition.hpp"

#include "engine/core/file_io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::converter {
namespace {

// FNV-1a, 64 bit. The hash is a change detector for the manifest, not a security measure.
[[nodiscard]] std::uint64_t fnv1a(std::string_view bytes, std::uint64_t seed = 14695981039346656037ull) noexcept {
    std::uint64_t hash = seed;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard]] std::string hex(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

// Blob fields are written in host byte order, which is little-endian on every supported target (x86-64 Windows and Linux).
template <typename T>
void append_value(std::string& out, const T& value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

[[nodiscard]] std::string build_terrain_blob(const std::vector<const world::TerrainTileMesh*>& tiles, std::uint32_t lod) {
    std::string out(kTerrainLodMagic);
    append_value(out, kTerrainLodBlobVersion);
    append_value(out, lod);
    append_value(out, static_cast<std::uint32_t>(tiles.size()));
    for (const auto* tile : tiles) {
        append_value(out, static_cast<std::int32_t>(tile->tile.x));
        append_value(out, static_cast<std::int32_t>(tile->tile.z));
        append_value(out, static_cast<std::uint32_t>(tile->vertices.size()));
        append_value(out, static_cast<std::uint32_t>(tile->indices.size()));
        for (const auto& vertex : tile->vertices) {
            for (const float component : {vertex.position.x, vertex.position.y, vertex.position.z, vertex.normal.x, vertex.normal.y,
                                          vertex.normal.z, vertex.splat.x, vertex.splat.y, vertex.splat.z, vertex.splat.w}) {
                append_value(out, component);
            }
        }
        for (const auto index : tile->indices) {
            append_value(out, index);
        }
    }
    return out;
}

[[nodiscard]] std::string format_float(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

}  // namespace

core::Result<PreparedWorld, std::string> prepare_world(const world::World& world, const world::Terrain* terrain,
                                                       const WorldPrepareOptions& options) {
    if (const auto problems = world::validate_world(world); !problems.empty()) {
        return core::failure("world is invalid: " + problems.front());
    }

    // Terrain tiles, grouped by the index of the cell that owns each tile's origin corner.
    std::vector<std::vector<const world::TerrainTileMesh*>> tiles_by_cell(world.cells.size());
    std::vector<world::TerrainTileMesh> meshes;
    std::uint32_t lod = 0;
    if (terrain != nullptr) {
        lod = std::min(options.distant_lod, world::max_terrain_lod(terrain->desc));
        const std::vector<std::uint32_t> tile_lods(
            static_cast<std::size_t>(terrain->desc.tiles_x) * terrain->desc.tiles_z, lod);
        auto built = world::build_terrain_meshes(*terrain, tile_lods);
        if (!built.ok()) {
            return core::failure("terrain LOD build failed: " + built.error());
        }
        meshes = std::move(built.value());
        const float tile_size = static_cast<float>(terrain->desc.tile_cells) * terrain->desc.cell_size;
        for (const auto& mesh : meshes) {
            const glm::vec2 origin(static_cast<float>(mesh.tile.x) * tile_size, static_cast<float>(mesh.tile.z) * tile_size);
            const world::CellCoord owner = world::cell_coord_at(world.desc, origin);
            const auto found = std::find_if(world.cells.begin(), world.cells.end(),
                                            [&](const world::WorldCell& cell) { return cell.coord == owner; });
            if (found == world.cells.end()) {
                return core::failure("terrain tile (" + std::to_string(mesh.tile.x) + ", " + std::to_string(mesh.tile.z) +
                                     ") has no owning cell at (" + std::to_string(owner.x) + ", " + std::to_string(owner.z) + ")");
            }
            tiles_by_cell[static_cast<std::size_t>(found - world.cells.begin())].push_back(&mesh);
        }
    }

    PreparedWorld prepared;
    prepared.layer_text = world::serialize_layer(world.layer);
    prepared.layer_objects = world.layer.objects.size();
    prepared.layer_hash = hex(fnv1a(prepared.layer_text));

    for (std::size_t c = 0; c < world.cells.size(); ++c) {
        const world::WorldCell& cell = world.cells[c];
        PreparedCell entry;
        entry.id = cell.id;
        entry.coord = cell.coord;
        entry.object_count = cell.objects.size();
        entry.terrain_tiles = tiles_by_cell[c].size();
        entry.cell_text = world::serialize_cell(cell);
        if (!tiles_by_cell[c].empty()) {
            entry.terrain_lod = lod;
            entry.terrain_blob = build_terrain_blob(tiles_by_cell[c], lod);
        }
        entry.hash = hex(fnv1a(entry.terrain_blob, fnv1a(entry.cell_text)));
        prepared.cells.push_back(std::move(entry));
    }
    std::sort(prepared.cells.begin(), prepared.cells.end(),
              [](const PreparedCell& a, const PreparedCell& b) { return a.id < b.id; });

    std::string& manifest = prepared.manifest;
    manifest = std::string(kWorldManifestFormat) + " " + std::to_string(kWorldManifestVersion) + "\n";
    manifest += "grid " + format_float(world.desc.origin.x) + " " + format_float(world.desc.origin.y) + " " +
                format_float(world.desc.cell_size) + "\n";
    manifest += "layer " + std::to_string(prepared.layer_objects) + " " + prepared.layer_hash + "\n";
    for (const auto& entry : prepared.cells) {
        manifest += "cell " + entry.id.to_string() + " " + std::to_string(entry.coord.x) + " " + std::to_string(entry.coord.z) + " " +
                    std::to_string(entry.object_count) + " " + std::to_string(entry.terrain_tiles) + " " +
                    std::to_string(entry.terrain_lod) + " " + entry.hash + "\n";
    }
    return core::success(std::move(prepared));
}

core::Result<core::Unit, std::string> publish_world(const PreparedWorld& prepared, const core::Path& directory) {
    if (auto made = core::create_directories(directory); !made.ok()) {
        return core::failure(core::describe(made.error()));
    }
    const auto write = [&](const std::string& name, std::string_view contents) -> core::Result<core::Unit, std::string> {
        auto path = directory.join(name);
        if (!path.ok()) {
            return core::failure("output file name is not a valid path: " + name);
        }
        if (auto saved = core::write_file_atomic(path.value(), contents); !saved.ok()) {
            return core::failure(core::describe(saved.error()));
        }
        return core::success();
    };
    for (const auto& entry : prepared.cells) {
        const std::string stem = entry.id.to_string();
        if (auto saved = write(stem + ".cell", entry.cell_text); !saved.ok()) {
            return saved;
        }
        if (!entry.terrain_blob.empty()) {
            if (auto saved = write(stem + ".terrain_lod", entry.terrain_blob); !saved.ok()) {
                return saved;
            }
        }
    }
    if (auto saved = write("layer.world", prepared.layer_text); !saved.ok()) {
        return saved;
    }
    return write(std::string(kWorldManifestName), prepared.manifest);
}

}  // namespace engine::converter
