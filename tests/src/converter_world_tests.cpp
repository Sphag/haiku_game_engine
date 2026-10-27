// World preparation in the converter (US-048). These cases check that every terrain tile is prepared by exactly one cell (the owner of its
// origin corner), that the distant LOD is clamped and encoded in the blob, that the manifest hash follows a change across a boundary, and
// that publishing writes exactly the prepared files.

#include "asset_converter/world_partition.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/terrain.hpp"
#include "engine/world/terrain_edit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using engine::converter::PreparedWorld;
using engine::converter::WorldPrepareOptions;
using engine::converter::prepare_world;
using engine::converter::publish_world;
using engine::core::Guid;
using engine::core::Path;
using engine::world::TerrainDesc;
using engine::world::World;
using engine::world::WorldDesc;
using engine::world::WorldObject;
namespace fs = std::filesystem;

namespace {

// Two by two tiles of 16 cells each: the terrain covers a 32 m square, and each tile is 16 m wide.
engine::world::Terrain small_terrain() {
    TerrainDesc desc;
    desc.tiles_x = 2;
    desc.tiles_z = 2;
    desc.tile_cells = 16;
    desc.cell_size = 1.0f;
    auto terrain = engine::world::create_terrain(desc);
    REQUIRE(terrain.ok());
    return std::move(terrain.value());
}

// A world whose cells are 16 m wide, so the four cells line up with the four tiles.
World aligned_world() {
    WorldDesc desc;
    desc.cell_size = 16.0f;
    auto created = engine::world::create_world(desc);
    REQUIRE(created.ok());
    World world = std::move(created.value());
    for (int z = 0; z < 2; ++z) {
        for (int x = 0; x < 2; ++x) {
            static_cast<void>(engine::world::ensure_cell(world, engine::world::CellCoord{x, z}));
        }
    }
    return world;
}

WorldObject crate_at(glm::vec3 position) {
    WorldObject object;
    object.id = Guid::generate();
    object.name = "crate";
    object.asset = Guid::generate();
    object.position = position;
    return object;
}

std::uint32_t read_u32(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::int32_t read_i32(const std::string& bytes, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Scratch directory removed at the end of the case.
class Scratch {
public:
    Scratch() : root_(fs::temp_directory_path() / ("engine_world_prep_" + Guid::generate().to_string())) {
        fs::create_directories(root_);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept {
        return root_;
    }

private:
    fs::path root_;
};

}  // namespace

TEST_CASE("Each terrain tile is prepared by the one cell that owns its origin corner", "[unit]") {
    const auto terrain = small_terrain();
    const World world = aligned_world();

    auto prepared = prepare_world(world, &terrain, WorldPrepareOptions{});
    REQUIRE(prepared.ok());
    REQUIRE(prepared.value().cells.size() == 4);

    std::size_t tiles = 0;
    for (const auto& cell : prepared.value().cells) {
        CHECK(cell.terrain_tiles == 1);
        tiles += cell.terrain_tiles;
        REQUIRE_FALSE(cell.terrain_blob.empty());
        CHECK(cell.terrain_blob.substr(0, 4) == "OWTL");
        CHECK(read_u32(cell.terrain_blob, 12) == 1);
        // The tile recorded in the blob is the one whose origin lies in this cell.
        const std::int32_t tile_x = read_i32(cell.terrain_blob, 16);
        const std::int32_t tile_z = read_i32(cell.terrain_blob, 20);
        CHECK(cell.coord.x == tile_x);
        CHECK(cell.coord.z == tile_z);
    }
    CHECK(tiles == 4);
}

TEST_CASE("A terrain tile with no owning cell is an error, not a silent drop", "[unit]") {
    const auto terrain = small_terrain();
    WorldDesc desc;
    desc.cell_size = 16.0f;
    auto created = engine::world::create_world(desc);
    REQUIRE(created.ok());
    World world = std::move(created.value());
    static_cast<void>(engine::world::ensure_cell(world, engine::world::CellCoord{0, 0}));

    CHECK_FALSE(prepare_world(world, &terrain, WorldPrepareOptions{}).ok());
}

TEST_CASE("The distant LOD is clamped to the terrain's maximum and recorded in each blob", "[unit]") {
    const auto terrain = small_terrain();
    const World world = aligned_world();
    const std::uint32_t max_lod = engine::world::max_terrain_lod(terrain.desc);

    WorldPrepareOptions options;
    options.distant_lod = 200;
    auto prepared = prepare_world(world, &terrain, options);
    REQUIRE(prepared.ok());
    for (const auto& cell : prepared.value().cells) {
        CHECK(cell.terrain_lod == max_lod);
        CHECK(read_u32(cell.terrain_blob, 8) == max_lod);
    }

    // Each blob holds exactly the mesh the terrain builder produces at that LOD.
    options.distant_lod = 1;
    auto at_one = prepare_world(world, &terrain, options);
    REQUIRE(at_one.ok());
    const std::vector<std::uint32_t> lods(4, 1);
    auto meshes = engine::world::build_terrain_meshes(terrain, lods);
    REQUIRE(meshes.ok());
    for (const auto& cell : at_one.value().cells) {
        for (const auto& mesh : meshes.value()) {
            if (mesh.tile.x != static_cast<std::uint32_t>(cell.coord.x) || mesh.tile.z != static_cast<std::uint32_t>(cell.coord.z)) {
                continue;
            }
            const std::size_t header_end = 32;  // magic, version, LOD, tile count, tile X and Z, vertex and index counts
            CHECK(read_u32(cell.terrain_blob, 24) == mesh.vertices.size());
            CHECK(read_u32(cell.terrain_blob, 28) == mesh.indices.size());
            CHECK(cell.terrain_blob.size() == header_end + 40 * mesh.vertices.size() + 4 * mesh.indices.size());
        }
    }
}

TEST_CASE("The manifest is deterministic and changes when an object crosses a cell boundary", "[unit]") {
    const auto terrain = small_terrain();
    World world = aligned_world();
    const auto crate = crate_at(glm::vec3(4.0f, 0.0f, 4.0f));
    REQUIRE(engine::world::add_object(world, crate, false).ok());

    auto first = prepare_world(world, &terrain, WorldPrepareOptions{});
    REQUIRE(first.ok());
    auto again = prepare_world(world, &terrain, WorldPrepareOptions{});
    REQUIRE(again.ok());
    CHECK(first.value().manifest == again.value().manifest);

    REQUIRE(engine::world::move_object(world, crate.id, glm::vec3(20.0f, 0.0f, 4.0f)).ok());
    auto moved = prepare_world(world, &terrain, WorldPrepareOptions{});
    REQUIRE(moved.ok());
    CHECK(moved.value().manifest != first.value().manifest);

    // Only the two cells that changed get a new hash. The tile count and LOD of every cell stay the same.
    std::size_t changed = 0;
    for (std::size_t i = 0; i < first.value().cells.size(); ++i) {
        const auto& before = first.value().cells[i];
        const auto& after = moved.value().cells[i];
        CHECK(before.id == after.id);
        CHECK(before.terrain_tiles == after.terrain_tiles);
        changed += before.hash != after.hash ? 1u : 0u;
    }
    CHECK(changed == 2);
}

TEST_CASE("Publishing writes the manifest, each cell, each terrain blob, and the layer, and nothing else", "[integration]") {
    const auto terrain = small_terrain();
    World world = aligned_world();
    WorldObject sky = crate_at(glm::vec3(0.0f));
    REQUIRE(engine::world::add_object(world, sky, true).ok());
    REQUIRE(engine::world::add_object(world, crate_at(glm::vec3(20.0f, 0.0f, 20.0f)), false).ok());

    auto prepared = prepare_world(world, &terrain, WorldPrepareOptions{});
    REQUIRE(prepared.ok());

    Scratch scratch;
    const auto root = Path::from_utf8((scratch.path() / "out").string());
    REQUIRE(root.ok());
    REQUIRE(publish_world(prepared.value(), root.value()).ok());

    // The blobs are binary, so they are read as raw bytes; the text reader rejects them.
    const auto read = [&](const std::string& name) {
        auto path = root.value().join(name);
        REQUIRE(path.ok());
        std::ifstream stream(engine::core::to_native_path(path.value()), std::ios::binary);
        REQUIRE(stream.good());
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    CHECK(read(std::string(engine::converter::kWorldManifestName)) == prepared.value().manifest);
    CHECK(read("layer.world") == prepared.value().layer_text);
    for (const auto& cell : prepared.value().cells) {
        CHECK(read(cell.id.to_string() + ".cell") == cell.cell_text);
        CHECK(read(cell.id.to_string() + ".terrain_lod") == cell.terrain_blob);
    }

    const std::size_t files = static_cast<std::size_t>(std::distance(fs::directory_iterator(scratch.path() / "out"), fs::directory_iterator{}));
    // Manifest, layer, four cells, and four terrain blobs.
    CHECK(files == 1 + 1 + 4 + 4);
}

TEST_CASE("An edited terrain saved, reopened, and cooked gives the same terrain blobs as the edit itself", "[integration]") {
    engine::world::TerrainDocument doc = engine::world::TerrainDocument::from_terrain(small_terrain());
    const World world = aligned_world();
    const std::string unedited = prepare_world(world, &doc.terrain(), WorldPrepareOptions{}).value().cells.front().terrain_blob;

    engine::world::Brush raise;
    raise.kind = engine::world::BrushKind::Raise;
    raise.radius = 6.0f;
    raise.strength = 1.0f;
    raise.falloff = 0.5f;
    REQUIRE(doc.begin_stroke(raise).ok());
    REQUIRE(doc.stamp(glm::vec2(16.0f, 16.0f)).ok());  // The shared corner of all four tiles.
    REQUIRE(doc.end_stroke().ok());
    auto edited = prepare_world(world, &doc.terrain(), WorldPrepareOptions{});
    REQUIRE(edited.ok());
    CHECK(edited.value().cells.front().terrain_blob != unedited);

    Scratch scratch;
    const auto file = Path::from_utf8((scratch.path() / "landscape.terrain").string());
    REQUIRE(file.ok());
    REQUIRE(engine::world::save_terrain(file.value(), doc.terrain()).ok());
    auto reopened = engine::world::load_terrain(file.value());
    REQUIRE(reopened.ok());

    auto cooked = prepare_world(world, &reopened.value(), WorldPrepareOptions{});
    REQUIRE(cooked.ok());
    REQUIRE(cooked.value().cells.size() == edited.value().cells.size());
    for (std::size_t i = 0; i < cooked.value().cells.size(); ++i) {
        CHECK(cooked.value().cells[i].terrain_blob == edited.value().cells[i].terrain_blob);
    }
    CHECK(cooked.value().manifest == edited.value().manifest);
}

