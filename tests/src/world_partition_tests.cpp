// World partition data (US-048), on the CPU. These cases check the half-open cell ownership on boundaries, the always-loaded layer, the
// cross-cell and layer references, moving an object across a boundary, the validation of hand-built state, the text round trip with
// its rejections, and saving only the cells that changed.

#include "engine/world/partition.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

using engine::core::Guid;
using engine::core::Path;
using engine::world::CellCoord;
using engine::world::WorldCell;
using engine::world::WorldDesc;
using engine::world::WorldObject;
using engine::world::add_object;
using engine::world::cell_bounds;
using engine::world::cell_coord_at;
using engine::world::create_world;
using engine::world::find_object;
using engine::world::move_object;
using engine::world::object_at;
using engine::world::parse_cell;
using engine::world::parse_layer;
using engine::world::save_dirty;
using engine::world::serialize_cell;
using engine::world::serialize_layer;
using engine::world::validate_world;
using engine::world::validate_world_desc;
namespace fs = std::filesystem;

namespace {

WorldDesc grid(float cell_size = 256.0f) {
    WorldDesc desc;
    desc.cell_size = cell_size;
    return desc;
}

WorldObject make_object(const std::string& name, glm::vec3 position, const Guid& asset) {
    WorldObject object;
    object.id = Guid::generate();
    object.name = name;
    object.asset = asset;
    object.position = position;
    return object;
}

// Adds an object and returns its GUID, failing the case when the world refuses it.
Guid add(engine::world::World& world, const WorldObject& object, bool always_loaded = false) {
    const auto added = add_object(world, object, always_loaded);
    REQUIRE(added.ok());
    return object.id;
}

// The number of places in the world that hold an object with this GUID. A single owner means exactly one.
std::size_t copies_of(const engine::world::World& world, const Guid& id) {
    std::size_t copies = 0;
    for (const auto& cell : world.cells) {
        for (const auto& object : cell.objects) {
            copies += object.id == id ? 1u : 0u;
        }
    }
    for (const auto& object : world.layer.objects) {
        copies += object.id == id ? 1u : 0u;
    }
    return copies;
}

bool same_object(const WorldObject& a, const WorldObject& b) {
    return a.id == b.id && a.name == b.name && a.asset == b.asset && a.position == b.position && a.references == b.references;
}

fs::path scratch_dir() {
    return fs::temp_directory_path() / ("engine_world_" + Guid::generate().to_string());
}

}  // namespace

TEST_CASE("The world description rejects a non-positive, infinite, or oversized cell size", "[unit]") {
    CHECK(validate_world_desc(grid(256.0f)).ok());
    CHECK_FALSE(validate_world_desc(grid(0.0f)).ok());
    CHECK_FALSE(validate_world_desc(grid(-4.0f)).ok());
    CHECK_FALSE(validate_world_desc(grid(std::numeric_limits<float>::infinity())).ok());
    CHECK_FALSE(validate_world_desc(grid(std::numeric_limits<float>::quiet_NaN())).ok());
    CHECK_FALSE(validate_world_desc(grid(1.0e6f)).ok());
    CHECK_FALSE(create_world(grid(0.0f)).ok());
}

TEST_CASE("A boundary point belongs to the cell whose index starts at that coordinate", "[unit]") {
    const WorldDesc desc = grid(256.0f);
    CHECK(cell_coord_at(desc, glm::dvec2(0.0f, 0.0f)) == CellCoord{0, 0});
    CHECK(cell_coord_at(desc, glm::dvec2(255.99f, 0.0f)) == CellCoord{0, 0});
    CHECK(cell_coord_at(desc, glm::dvec2(256.0f, 0.0f)) == CellCoord{1, 0});
    CHECK(cell_coord_at(desc, glm::dvec2(-0.0001f, 0.0f)) == CellCoord{-1, 0});
    CHECK(cell_coord_at(desc, glm::dvec2(-256.0f, -256.0f)) == CellCoord{-1, -1});

    WorldDesc shifted = grid(256.0f);
    shifted.origin = glm::vec2(100.0f, 100.0f);
    CHECK(cell_coord_at(shifted, glm::dvec2(100.0f, 100.0f)) == CellCoord{0, 0});
    CHECK(cell_coord_at(shifted, glm::dvec2(356.0f, 100.0f)) == CellCoord{1, 0});

    const auto bounds = cell_bounds(desc, CellCoord{1, 0});
    CHECK(bounds.min.x == Catch::Approx(256.0f));
    CHECK(bounds.max.x == Catch::Approx(512.0f));
    CHECK(cell_bounds(desc, CellCoord{0, 0}).max.x == cell_bounds(desc, CellCoord{1, 0}).min.x);
}

TEST_CASE("An object on a cell boundary has exactly one owner, and the owner is the cell beyond the boundary", "[unit]") {
    auto created = create_world(grid(256.0f));
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();

    const auto on_boundary = make_object("edge", glm::vec3(256.0f, 0.0f, 0.0f), asset);
    const Guid id = add(world, on_boundary);
    add(world, make_object("west", glm::vec3(255.0f, 0.0f, 0.0f), asset));

    REQUIRE(copies_of(world, id) == 1);
    const auto location = find_object(world, id);
    REQUIRE(location.has_value());
    CHECK_FALSE(location->in_layer);
    CHECK(world.cells[location->cell].coord == CellCoord{1, 0});
    CHECK(validate_world(world).empty());
}

TEST_CASE("The always-loaded layer holds its objects without creating a cell", "[unit]") {
    auto created = create_world(grid());
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();

    const Guid sky = add(world, make_object("sky", glm::vec3(900.0f, 40.0f, -900.0f), asset), true);
    CHECK(world.cells.empty());
    REQUIRE(world.layer.objects.size() == 1);

    const auto location = find_object(world, sky);
    REQUIRE(location.has_value());
    CHECK(location->in_layer);
    CHECK(object_at(world, *location).name == "sky");
    CHECK(validate_world(world).empty());
}

TEST_CASE("A reference resolves across cells and into the layer, and a dangling reference is reported", "[unit]") {
    auto created = create_world(grid(256.0f));
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();

    const Guid lamp = add(world, make_object("lamp", glm::vec3(10.0f, 0.0f, 10.0f), asset));
    const Guid beacon = add(world, make_object("beacon", glm::vec3(600.0f, 0.0f, 10.0f), asset), true);

    auto gate = make_object("gate", glm::vec3(300.0f, 0.0f, 10.0f), asset);
    gate.references = {lamp, beacon};
    const Guid gate_id = add(world, gate);
    CHECK(validate_world(world).empty());

    // A reference to an object that was never added is a problem, and it names the referencing object.
    auto orphan = make_object("orphan", glm::vec3(20.0f, 0.0f, 20.0f), asset);
    orphan.references = {Guid::generate()};
    add(world, orphan);
    const auto problems = validate_world(world);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().find(orphan.id.to_string()) != std::string::npos);
    CHECK(find_object(world, gate_id).has_value());
}

TEST_CASE("Moving an object across a boundary keeps its identity and marks both cells dirty", "[unit]") {
    auto created = create_world(grid(256.0f));
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();

    const Guid anchor = add(world, make_object("anchor", glm::vec3(50.0f, 0.0f, 50.0f), asset));
    auto crate = make_object("crate", glm::vec3(100.0f, 0.0f, 100.0f), asset);
    crate.references = {anchor};
    const Guid id = add(world, crate);
    world.cells.front().dirty = false;  // Pretend the origin cell was just saved.
    const std::size_t origin_index = find_object(world, id)->cell;
    const Guid origin_cell = world.cells[origin_index].id;
    const WorldObject before = object_at(world, *find_object(world, id));

    REQUIRE(move_object(world, id, glm::vec3(300.0f, 0.0f, 100.0f)).ok());

    REQUIRE(copies_of(world, id) == 1);
    const auto after_location = find_object(world, id);
    REQUIRE(after_location.has_value());
    const WorldCell& destination = world.cells[after_location->cell];
    CHECK(destination.coord == CellCoord{1, 0});
    CHECK(destination.id != origin_cell);
    CHECK(destination.dirty);
    CHECK(world.cells[origin_index].dirty);
    CHECK(world.cells[origin_index].coord == CellCoord{0, 0});

    const WorldObject& moved = object_at(world, *after_location);
    CHECK(moved.position == glm::vec3(300.0f, 0.0f, 100.0f));
    CHECK(moved.id == before.id);
    CHECK(moved.name == before.name);
    CHECK(moved.asset == before.asset);
    CHECK(moved.references == before.references);
    CHECK(copies_of(world, anchor) == 1);
    CHECK(validate_world(world).empty());
}

TEST_CASE("Moving within a cell keeps the owner, and moving a layer object never changes its place", "[unit]") {
    auto created = create_world(grid(256.0f));
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();

    const Guid barrel = add(world, make_object("barrel", glm::vec3(10.0f, 0.0f, 10.0f), asset));
    const std::size_t cell_count = world.cells.size();
    REQUIRE(move_object(world, barrel, glm::vec3(20.0f, 0.0f, 30.0f)).ok());
    CHECK(world.cells.size() == cell_count);
    CHECK(object_at(world, *find_object(world, barrel)).position == glm::vec3(20.0f, 0.0f, 30.0f));

    const Guid beacon = add(world, make_object("beacon", glm::vec3(0.0f), asset), true);
    REQUIRE(move_object(world, beacon, glm::vec3(5000.0f, 0.0f, -5000.0f)).ok());
    const auto location = find_object(world, beacon);
    REQUIRE(location.has_value());
    CHECK(location->in_layer);
    CHECK(world.cells.size() == cell_count);
    CHECK(world.layer.dirty);

    CHECK_FALSE(move_object(world, Guid::generate(), glm::vec3(0.0f)).ok());
}

TEST_CASE("Duplicate GUIDs, nil assets, multi-line names, and non-finite positions are rejected", "[unit]") {
    auto created = create_world(grid());
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();

    const auto first = make_object("a", glm::vec3(1.0f), asset);
    add(world, first);
    CHECK_FALSE(add_object(world, first, false).ok());
    CHECK_FALSE(add_object(world, first, true).ok());

    CHECK_FALSE(add_object(world, make_object("nil asset", glm::vec3(1.0f), Guid{}), false).ok());
    CHECK_FALSE(add_object(world, make_object("two\nlines", glm::vec3(1.0f), asset), false).ok());
    CHECK_FALSE(add_object(world, make_object("nan", glm::vec3(std::nanf(""), 0.0f, 0.0f), asset), false).ok());
    CHECK_FALSE(add_object(world, make_object("far", glm::vec3(1.0e9f, 0.0f, 0.0f), asset), false).ok());
    CHECK_FALSE(move_object(world, first.id, glm::vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f)).ok());
    CHECK(copies_of(world, first.id) == 1);
}

TEST_CASE("Validation flags a misplaced object, a missing asset dependency, and a duplicated cell", "[unit]") {
    auto created = create_world(grid(256.0f));
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();
    add(world, make_object("ok", glm::vec3(10.0f, 0.0f, 10.0f), asset));
    REQUIRE(validate_world(world).empty());

    // Build broken state directly, the way a hand-edited or corrupt file could.
    WorldObject misplaced = make_object("misplaced", glm::vec3(900.0f, 0.0f, 10.0f), asset);
    world.cells.front().objects.push_back(misplaced);
    WorldObject unlisted = make_object("unlisted", glm::vec3(20.0f, 0.0f, 20.0f), Guid::generate());
    world.cells.front().objects.push_back(unlisted);
    WorldCell twin = world.cells.front();
    twin.id = Guid::generate();
    world.cells.push_back(twin);

    const auto problems = validate_world(world);
    CHECK(problems.size() >= 3);
}

TEST_CASE("A cell and the layer round-trip through their text form, including names and references", "[unit]") {
    auto created = create_world(grid(256.0f));
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();

    auto tower = make_object("  tall tower, north  ", glm::vec3(12.5f, -3.25f, 200.125f), asset);
    tower.references = {Guid::generate()};
    add(world, tower);
    add(world, make_object("", glm::vec3(1.0f / 3.0f, 0.0f, 2.0f), asset));
    add(world, make_object("beacon", glm::vec3(4.0f, 9.0f, -7.0f), asset), true);

    const WorldCell& cell = world.cells.front();
    const auto parsed_cell = parse_cell(serialize_cell(cell));
    REQUIRE(parsed_cell.ok());
    CHECK(parsed_cell.value().id == cell.id);
    CHECK(parsed_cell.value().coord == cell.coord);
    CHECK(parsed_cell.value().assets == cell.assets);
    REQUIRE(parsed_cell.value().objects.size() == cell.objects.size());
    for (std::size_t i = 0; i < cell.objects.size(); ++i) {
        CHECK(same_object(parsed_cell.value().objects[i], cell.objects[i]));
    }

    const auto parsed_layer = parse_layer(serialize_layer(world.layer));
    REQUIRE(parsed_layer.ok());
    REQUIRE(parsed_layer.value().objects.size() == 1);
    CHECK(same_object(parsed_layer.value().objects.front(), world.layer.objects.front()));
}

TEST_CASE("The parsers reject another schema version, an unknown keyword, and a truncated file", "[unit]") {
    auto created = create_world(grid());
    REQUIRE(created.ok());
    auto& world = created.value();
    add(world, make_object("crate", glm::vec3(1.0f), Guid::generate()));
    const std::string text = serialize_cell(world.cells.front());

    std::string other_version = text;
    other_version.replace(other_version.find(" 1\n"), 3, " 2\n");
    CHECK_FALSE(parse_cell(other_version).ok());

    std::string unknown = text;
    unknown.insert(unknown.find("end\n"), "bogus 1\n");
    CHECK_FALSE(parse_cell(unknown).ok());

    std::string dangling_ref = text;
    dangling_ref.insert(dangling_ref.find("end\n"), "ref " + Guid::generate().to_string() + "\n");
    CHECK(parse_cell(dangling_ref).ok());  // The syntax is valid; resolving the GUID is validate_world's job.

    std::string truncated = text;
    truncated.erase(truncated.find("end\n"));
    CHECK_FALSE(parse_cell(truncated).ok());

    CHECK_FALSE(parse_layer(text).ok());  // A cell file is not a layer file.
    CHECK_FALSE(parse_cell("").ok());
}

TEST_CASE("Saving writes only the cells that changed, and a reloaded cell matches the saved one", "[integration]") {
    auto created = create_world(grid(256.0f));
    REQUIRE(created.ok());
    auto& world = created.value();
    const Guid asset = Guid::generate();
    add(world, make_object("west", glm::vec3(10.0f, 0.0f, 10.0f), asset));
    add(world, make_object("east", glm::vec3(300.0f, 0.0f, 10.0f), asset));
    add(world, make_object("sky", glm::vec3(0.0f), asset), true);

    const fs::path directory = scratch_dir();
    const auto root = Path::from_utf8(directory.string());
    REQUIRE(root.ok());
    const auto first = save_dirty(world, root.value());
    REQUIRE(first.ok());
    CHECK(first.value() == 3);  // Two cells and the layer.
    for (const auto& cell : world.cells) {
        CHECK_FALSE(cell.dirty);
    }
    CHECK_FALSE(world.layer.dirty);

    const auto unchanged = save_dirty(world, root.value());
    REQUIRE(unchanged.ok());
    CHECK(unchanged.value() == 0);

    // Edit one object in the east cell. Only that cell is dirty, so only its file is written.
    const Guid east = world.cells.back().objects.front().id;
    REQUIRE(move_object(world, east, glm::vec3(320.0f, 0.0f, 40.0f)).ok());
    const auto edited = save_dirty(world, root.value());
    REQUIRE(edited.ok());
    CHECK(edited.value() == 1);

    // Each saved cell reloads to the in-memory cell.
    for (const auto& cell : world.cells) {
        const auto path = root.value().join(engine::world::cell_file_name(cell));
        REQUIRE(path.ok());
        const auto reloaded = engine::world::load_cell(path.value());
        REQUIRE(reloaded.ok());
        CHECK(reloaded.value().id == cell.id);
        REQUIRE(reloaded.value().objects.size() == cell.objects.size());
        for (std::size_t i = 0; i < cell.objects.size(); ++i) {
            CHECK(same_object(reloaded.value().objects[i], cell.objects[i]));
        }
    }
    const auto layer_path = root.value().join(std::string(engine::world::kLayerFileName));
    REQUIRE(layer_path.ok());
    const auto layer = engine::world::load_layer(layer_path.value());
    REQUIRE(layer.ok());
    CHECK(layer.value().objects.size() == 1);

    std::error_code ignored;
    fs::remove_all(directory, ignored);
}
