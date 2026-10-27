// Game saves (US-059) on the CPU: component selection and GUIDs, rejection of corrupt and incompatible files, slot writes that keep the
// previous save, script state captured and restored through Lua, and world changes restored into cells and cell files. No renderer or
// physics world is needed, so these run on every operating system.

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/jobs.hpp"
#include "engine/core/path.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/save/save.hpp"
#include "engine/save/world_changes.hpp"
#include "engine/scene/reflection.hpp"
#include "engine/scene/scene_format.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/script/script_runner.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

using engine::core::Guid;
using engine::core::Path;
using engine::save::SaveData;
using engine::save::SaveError;
using engine::save::SaveErrorCode;
using engine::save::SaveSlots;
using engine::save::ScriptSnapshot;
using engine::save::WorldChanges;
using engine::scene::ComponentBuilder;
using engine::scene::ComponentCatalog;
using engine::scene::Range;
using engine::scene::parse_scene;
using engine::scene::serialize_scene;
using engine::script::ScriptEngine;
using engine::script::ScriptRunner;
using engine::script::ScriptServices;
using engine::world::WorldObject;
using engine::world::World;

namespace {

struct DoorLock {
    bool locked = true;
    float strength = 1.0f;
};

struct Health {
    std::int32_t hit_points = 100;
};

ComponentCatalog make_catalog() {
    ComponentCatalog catalog;
    ComponentBuilder<DoorLock> door("game.door_lock", 1);
    door.field<&DoorLock::locked>("locked", true);
    door.field<&DoorLock::strength>("strength", 1.0f, Range{0.0, 10.0});
    auto built = door.build();
    REQUIRE(built.ok());
    REQUIRE(catalog.add(built.value()).ok());

    ComponentBuilder<Health> health("game.health", 2);
    health.field<&Health::hit_points>("hit_points", 100, Range{0.0, 1000.0});
    auto health_built = health.build();
    REQUIRE(health_built.ok());
    REQUIRE(catalog.add(health_built.value()).ok());
    return catalog;
}

constexpr const char* kParentGuid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301";
constexpr const char* kChildGuid = "6b1c2e4a-9d3f-4e8b-8a7c-1f2e3d4c5b6a";

// Two entities: a door lock and health on the parent, health alone on the child.
constexpr const char* kSceneText = R"({
  "format": "engine.scene",
  "version": 1,
  "entities": [
    {"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301", "parent": null,
     "transform": {"position": [1, 2, 3], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1]},
     "components": {
       "game.door_lock": {"schema": 1, "fields": {"locked": false, "strength": 2.5}},
       "game.health": {"schema": 2, "fields": {"hit_points": 40}}}},
    {"guid": "6b1c2e4a-9d3f-4e8b-8a7c-1f2e3d4c5b6a", "parent": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
     "transform": {"position": [0, 0, 0], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1]},
     "components": {"game.health": {"schema": 2, "fields": {"hit_points": 5}}}}
  ]
})";

Guid guid_of(std::string_view text) {
    const auto parsed = Guid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

// Replaces the first occurrence. The save keys are sorted, so "version" is the last top-level key: a test that damages it matches
// "\"version\": 1\n}", which the scene's own "version" does not end with.
std::string replace_first(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// A save with the selected components of the test scene and one script and one world cell.
SaveData make_save(const ComponentCatalog& catalog, const Guid& save_id = Guid::generate()) {
    auto document = parse_scene(kSceneText, catalog);
    REQUIRE(document.ok());
    SaveData data;
    data.save_id = save_id;
    data.gameplay = engine::save::select_components(document.value(), {"game.health"});
    data.scripts.push_back(ScriptSnapshot{guid_of(kChildGuid), "door", R"({"open":true,"hp":3})"});
    return data;
}

std::string text_of(const SaveData& data) {
    auto text = engine::save::serialize_save(data);
    REQUIRE(text.ok());
    return text.value();
}

// A scratch directory that is removed at the end of the test.
struct Scratch {
    Path path;

    Scratch() : path(make_path()) {}
    ~Scratch() {
        static_cast<void>(engine::core::remove_all(path));
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

private:
    static Path make_path() {
        auto base = Path::from_utf8(std::filesystem::temp_directory_path().string());
        REQUIRE(base.ok());
        auto dir = base.value().join("engine_save_tests_" + Guid::generate().to_string());
        REQUIRE(dir.ok());
        REQUIRE(engine::core::create_directories(dir.value()).ok());
        return dir.value();
    }
};

// A script engine and runner with the registry they need.
struct LuaHost {
    engine::ecs::Registry registry;
    ScriptEngine engine;
    ScriptRunner runner{engine, registry};

    LuaHost() {
        ScriptServices services;
        services.registry = &registry;
        engine.bind(services);
    }

    void run_ok(const char* source) {
        const auto result = engine.run("test", source);
        const std::string detail = result.ok() ? std::string() : result.error().message;
        INFO(detail);
        REQUIRE(result.ok());
    }
};

// The authored world: a crate in cell (0, 0) and a lamp in cell (1, 0). Both cells are clean, as after loading from disk.
World make_authored_world(WorldObject& crate, WorldObject& lamp) {
    engine::world::WorldDesc desc;
    desc.cell_size = 256.0f;
    auto created = engine::world::create_world(desc);
    REQUIRE(created.ok());
    World world = created.value();

    const Guid asset = Guid::generate();
    crate = WorldObject{Guid::generate(), "crate", asset, glm::vec3(10.0f, 0.0f, 10.0f), {}};
    lamp = WorldObject{Guid::generate(), "lamp", asset, glm::vec3(300.0f, 0.0f, 10.0f), {}};
    REQUIRE(engine::world::add_object(world, crate, false).ok());
    REQUIRE(engine::world::add_object(world, lamp, false).ok());
    for (auto& cell : world.cells) {
        cell.dirty = false;
    }
    return world;
}

}  // namespace

TEST_CASE("A save keeps only the selected components, the GUIDs, and the hierarchy", "[unit]") {
    const ComponentCatalog catalog = make_catalog();
    auto document = parse_scene(kSceneText, catalog);
    REQUIRE(document.ok());
    const SaveData data = make_save(catalog, guid_of("0f1e2d3c-4b5a-4968-8778-6a5b4c3d2e1f"));

    const std::string text = text_of(data);
    auto parsed = engine::save::parse_save(text, catalog);
    REQUIRE(parsed.ok());

    CHECK(parsed.value().save_id == data.save_id);
    CHECK(serialize_scene(parsed.value().gameplay) == serialize_scene(data.gameplay));
    REQUIRE(parsed.value().gameplay.entities.size() == 2);
    const auto& parent = parsed.value().gameplay.entities[0];
    const auto& child = parsed.value().gameplay.entities[1];
    CHECK(parent.guid == guid_of(kParentGuid));
    CHECK(child.guid == guid_of(kChildGuid));
    CHECK(child.parent == parent.guid);
    CHECK(parent.components.count("game.door_lock") == 0);
    CHECK(parent.components.count("game.health") == 1);
    CHECK(text.find("game.door_lock") == std::string::npos);
    CHECK(text.find("game.health") != std::string::npos);
}

TEST_CASE("A save restores the script state and the entity it belongs to", "[unit]") {
    const ComponentCatalog catalog = make_catalog();
    const SaveData data = make_save(catalog);

    auto parsed = engine::save::parse_save(text_of(data), catalog);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().scripts.size() == 1);
    CHECK(parsed.value().scripts[0].entity == guid_of(kChildGuid));
    CHECK(parsed.value().scripts[0].chunk == "door");
    CHECK(parsed.value().scripts[0].state == R"({"hp":3,"open":true})");
}

TEST_CASE("A save with another format name, another version, or a damaged body is rejected", "[unit]") {
    const ComponentCatalog catalog = make_catalog();
    const std::string text = text_of(make_save(catalog));

    auto wrong_format = engine::save::parse_save(replace_first(text, "\"engine.save\"", "\"engine.other\""), catalog);
    REQUIRE_FALSE(wrong_format.ok());
    CHECK(wrong_format.error().code == SaveErrorCode::WrongFormat);

    auto future_version = engine::save::parse_save(replace_first(text, "\"version\": 1\n}", "\"version\": 2\n}"), catalog);
    REQUIRE_FALSE(future_version.ok());
    CHECK(future_version.error().code == SaveErrorCode::UnsupportedVersion);

    // A changed value keeps the file valid JSON, so only the checksum can catch it.
    auto altered = engine::save::parse_save(replace_first(text, "\"hit_points\": 40", "\"hit_points\": 41"), catalog);
    REQUIRE_FALSE(altered.ok());
    CHECK(altered.error().code == SaveErrorCode::ChecksumMismatch);

    auto truncated = engine::save::parse_save(text.substr(0, text.size() / 2), catalog);
    REQUIRE_FALSE(truncated.ok());
    CHECK(truncated.error().code == SaveErrorCode::Malformed);

    auto bad_checksum = engine::save::parse_save(replace_first(text, "\"checksum\": \"crc32:", "\"checksum\": \"crc32:zz"), catalog);
    REQUIRE_FALSE(bad_checksum.ok());
    CHECK(bad_checksum.error().code == SaveErrorCode::Malformed);

    auto not_json = engine::save::parse_save("save", catalog);
    REQUIRE_FALSE(not_json.ok());
    CHECK(not_json.error().code == SaveErrorCode::Malformed);
}

TEST_CASE("A slot keeps the previous save, and a failed write leaves the slot as it was", "[unit]") {
    const ComponentCatalog catalog = make_catalog();
    Scratch scratch;
    const SaveSlots slots(scratch.path);

    SaveData first = make_save(catalog);
    auto written = slots.write("autosave", first);
    REQUIRE(written.ok());
    CHECK(written.value() == first.save_id);
    CHECK(slots.exists("autosave"));

    // A rewrite keeps the GUID the slot already holds, and the replaced save becomes the previous one.
    SaveData second = make_save(catalog, first.save_id);
    second.gameplay.entities[1].components["game.health"].fields["hit_points"] = engine::scene::FieldValue{std::int32_t{99}};
    REQUIRE(slots.write("autosave", second).ok());

    auto current = slots.read("autosave", catalog);
    REQUIRE(current.ok());
    CHECK(current.value().save_id == first.save_id);
    CHECK(serialize_scene(current.value().gameplay) == serialize_scene(second.gameplay));

    auto previous = slots.read_previous("autosave", catalog);
    REQUIRE(previous.ok());
    CHECK(serialize_scene(previous.value().gameplay) == serialize_scene(first.gameplay));

    // A save that cannot be serialized fails before the file is touched, so the slot still holds the second save.
    SaveData broken = make_save(catalog);
    broken.scripts[0].state = "[1, 2]";
    auto failed = slots.write("autosave", broken);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == SaveErrorCode::InvalidContent);
    auto still = slots.read("autosave", catalog);
    REQUIRE(still.ok());
    CHECK(serialize_scene(still.value().gameplay) == serialize_scene(second.gameplay));
}

TEST_CASE("A corrupt slot is rejected, and the last good save survives the next write", "[unit]") {
    const ComponentCatalog catalog = make_catalog();
    Scratch scratch;
    const SaveSlots slots(scratch.path);

    const SaveData good = make_save(catalog);
    REQUIRE(slots.write("slot", good).ok());
    REQUIRE(slots.write("slot", make_save(catalog)).ok());

    // Damage the file on disk, as a crash in another program might.
    const Path file = scratch.path.join("slot.save").value();
    REQUIRE(engine::core::write_file_atomic(file, "{\"format\": \"engine.save\"").ok());
    auto damaged = slots.read("slot", catalog);
    REQUIRE_FALSE(damaged.ok());
    CHECK(damaged.error().code == SaveErrorCode::Malformed);

    // The damaged file is not copied over the previous save, so the previous save still loads after the next write.
    REQUIRE(slots.write("slot", make_save(catalog)).ok());
    auto previous = slots.read_previous("slot", catalog);
    REQUIRE(previous.ok());
    CHECK(serialize_scene(previous.value().gameplay) == serialize_scene(good.gameplay));
}

TEST_CASE("A slot name is 1 to 64 characters of letters, digits, '_' and '-'", "[unit]") {
    const ComponentCatalog catalog = make_catalog();
    Scratch scratch;
    const SaveSlots slots(scratch.path);
    const SaveData data = make_save(catalog);

    CHECK(slots.write("", data).error().code == SaveErrorCode::InvalidSlot);
    CHECK(slots.write("../escape", data).error().code == SaveErrorCode::InvalidSlot);
    CHECK(slots.write("two words", data).error().code == SaveErrorCode::InvalidSlot);
    CHECK(slots.write(std::string(65, 'a'), data).error().code == SaveErrorCode::InvalidSlot);
    CHECK(slots.write(std::string(64, 'a'), data).ok());
    CHECK(slots.write("slot-1_b", data).ok());
}

TEST_CASE("A script's plain state is captured, and restored before its first update", "[unit]") {
    LuaHost first;
    auto attached = first.runner.attach(first.registry.create(), "game",
                                        "hp = 7; tags = {'a', 'b'}; flags = {lucky = true}; nested = {x = 2.5, list = {1, 2, 3}}\n"
                                        "function update(e, dt) end");
    REQUIRE(attached.ok());
    auto captured = first.runner.capture_state(attached.value());
    REQUIRE(captured.ok());
    // Callbacks come from the chunk, so the state holds data only.
    CHECK(captured.value().find("update") == std::string::npos);
    CHECK(captured.value().find("\"hp\":7") != std::string::npos);

    LuaHost second;
    auto restored_id = second.runner.attach(second.registry.create(), "game",
                                            "hp = 1; tags = {'x'}; flags = {}; nested = {x = 0, list = {}}\n"
                                            "function update(e, dt) _G.seen = hp; _G.check = tags[2] == 'b' and flags.lucky == true "
                                            "and nested.x == 2.5 and nested.list[3] == 3 end");
    REQUIRE(restored_id.ok());
    REQUIRE(second.runner.restore_state(restored_id.value(), captured.value()).ok());
    second.runner.begin_frame();
    second.runner.update(0.0);
    second.run_ok("assert(_G.seen == 7 and _G.check == true, 'restored: ' .. tostring(_G.seen))");
}

TEST_CASE("Script state that cannot be stored is refused, and a refused restore changes nothing", "[unit]") {
    LuaHost host;
    auto mixed = host.runner.attach(host.registry.create(), "mixed", "mixed = {1, lucky = true}");
    REQUIRE(mixed.ok());
    auto refused = host.runner.capture_state(mixed.value());
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message.find("only string keys") != std::string::npos);

    auto cyclic = host.runner.attach(host.registry.create(), "cycle", "loop = {}\nloop.self = loop");
    REQUIRE(cyclic.ok());
    REQUIRE_FALSE(host.runner.capture_state(cyclic.value()).ok());

    auto door = host.runner.attach(host.registry.create(), "door", "hp = 1\nfunction update(e, dt) _G.seen = hp end");
    REQUIRE(door.ok());
    // The bad value comes last in the text, so the good value before it must not reach the environment either.
    REQUIRE_FALSE(host.runner.restore_state(door.value(), R"({"hp": 3, "bad": null})").ok());
    REQUIRE_FALSE(host.runner.restore_state(door.value(), "[1]").ok());
    host.runner.begin_frame();
    host.runner.update(0.0);
    host.run_ok("assert(_G.seen == 1, 'a refused restore changed hp to ' .. tostring(_G.seen))");

    // Restore is for a Pending instance only, so values never change under a running script.
    REQUIRE_FALSE(host.runner.restore_state(door.value(), R"({"hp": 9})").ok());
}

TEST_CASE("Changed world cells are saved and restored into an authored world, and written as cell files", "[unit]") {
    WorldObject crate;
    WorldObject lamp;
    const World authored = make_authored_world(crate, lamp);

    // The running world moves the crate inside its cell, so only that cell is dirty.
    World runtime = authored;
    REQUIRE(engine::world::move_object(runtime, crate.id, glm::vec3(20.0f, 0.0f, 10.0f)).ok());
    const WorldChanges changes = engine::save::collect_world_changes(runtime);
    REQUIRE(changes.cells.size() == 1);
    CHECK_FALSE(changes.layer.has_value());

    SaveData data;
    data.save_id = Guid::generate();
    data.world = changes;
    const ComponentCatalog catalog = make_catalog();
    auto parsed = engine::save::parse_save(text_of(data), catalog);
    REQUIRE(parsed.ok());

    World restored = authored;
    REQUIRE(engine::save::apply_world_changes(restored, parsed.value().world).ok());
    CHECK(engine::world::validate_world(restored).empty());
    const auto location = engine::world::find_object(restored, crate.id);
    REQUIRE(location.has_value());
    CHECK(engine::world::object_at(restored, *location).position.x == 20.0f);
    const auto lamp_location = engine::world::find_object(restored, lamp.id);
    REQUIRE(lamp_location.has_value());
    CHECK(engine::world::object_at(restored, *lamp_location).position.x == 300.0f);
    CHECK(restored.cells[location->cell].dirty);

    // The restored cell is also written as a cell file, so a streaming manager that reads the directory sees the saved state.
    Scratch scratch;
    REQUIRE(engine::save::write_cell_files(scratch.path, parsed.value().world).ok());
    const Path cell_file = scratch.path.join(engine::world::cell_file_name(parsed.value().world.cells[0])).value();
    auto cell = engine::world::load_cell(cell_file);
    REQUIRE(cell.ok());
    REQUIRE(cell.value().objects.size() == 1);
    CHECK(cell.value().objects[0].position.x == 20.0f);
}

TEST_CASE("A world change that conflicts with the authored world leaves the world unchanged", "[unit]") {
    WorldObject crate;
    WorldObject lamp;
    World world = make_authored_world(crate, lamp);
    const std::size_t cells_before = world.cells.size();

    // A cell with a new GUID at the coordinate of an authored cell cannot be applied.
    WorldChanges conflicting;
    engine::world::WorldCell intruder;
    intruder.id = Guid::generate();
    intruder.coord = world.cells[0].coord;
    conflicting.cells.push_back(intruder);
    const auto conflict = engine::save::apply_world_changes(world, conflicting);
    REQUIRE_FALSE(conflict.ok());
    CHECK(conflict.error().code == SaveErrorCode::InvalidContent);
    CHECK(world.cells.size() == cells_before);
    CHECK(engine::world::find_object(world, crate.id).has_value());

    // A new cell at a free coordinate is added, and its objects are checked as well.
    WorldChanges added;
    engine::world::WorldCell fresh;
    fresh.id = Guid::generate();
    fresh.coord = engine::world::CellCoord{5, 5};
    added.cells.push_back(fresh);
    REQUIRE(engine::save::apply_world_changes(world, added).ok());
    CHECK(world.cells.size() == cells_before + 1);
    CHECK(engine::world::validate_world(world).empty());
}

TEST_CASE("A save with a dirty layer restores the always-loaded objects", "[unit]") {
    WorldObject crate;
    WorldObject lamp;
    World world = make_authored_world(crate, lamp);
    WorldObject beacon{Guid::generate(), "beacon", crate.asset, glm::vec3(0.0f, 0.0f, 0.0f), {}};
    REQUIRE(engine::world::add_object(world, beacon, true).ok());

    const WorldChanges changes = engine::save::collect_world_changes(world);
    REQUIRE(changes.layer.has_value());
    CHECK(changes.cells.size() == 0);

    World authored = make_authored_world(crate, lamp);
    REQUIRE(engine::save::apply_world_changes(authored, changes).ok());
    CHECK(engine::world::find_object(authored, beacon.id).has_value());
    CHECK(authored.layer.dirty);
}

namespace {

// Keeps the cells it is given, so a test can read what the streaming manager loaded.
class LoadedCells : public engine::world::CellSubscriber {
public:
    std::vector<engine::world::CellData> cells;

    void on_loaded(const engine::world::CellData& cell) override {
        cells.push_back(cell);
    }
};

}  // namespace

TEST_CASE("A cell that streams in after a load carries the saved change", "[unit]") {
    WorldObject crate;
    WorldObject lamp;
    const World authored = make_authored_world(crate, lamp);

    // The runtime directory holds the authored cells, as a project copy would. Writing the save's cell files over it is what a load does.
    Scratch scratch;
    const Path authored_dir = scratch.path.join("authored").value();
    const Path runtime_dir = scratch.path.join("runtime").value();
    for (const Path& directory : {authored_dir, runtime_dir}) {
        World copy = authored;
        for (auto& cell : copy.cells) {
            cell.dirty = true;
        }
        REQUIRE(engine::world::save_dirty(copy, directory).ok());
    }

    World runtime = authored;
    REQUIRE(engine::world::move_object(runtime, crate.id, glm::vec3(20.0f, 0.0f, 10.0f)).ok());
    const WorldChanges changes = engine::save::collect_world_changes(runtime);
    REQUIRE(engine::save::write_cell_files(runtime_dir, changes).ok());

    engine::core::JobSystem jobs(2);
    engine::world::StreamingManager manager(jobs, runtime_dir, authored);
    auto recorder = std::make_shared<LoadedCells>();
    manager.subscribe(recorder);
    for (int round = 0; round < 12; ++round) {
        manager.update(glm::dvec2(128.0, 128.0));
        jobs.wait_idle();
        manager.safe_point();
    }
    REQUIRE(manager.phase(engine::world::CellCoord{0, 0}) == engine::world::CellPhase::Active);

    bool found = false;
    for (const auto& cell : recorder->cells) {
        if (cell->coord != engine::world::CellCoord{0, 0}) {
            continue;
        }
        found = true;
        REQUIRE(cell->objects.size() == 1);
        CHECK(cell->objects[0].id == crate.id);
        CHECK(cell->objects[0].position.x == 20.0f);
    }
    CHECK(found);
}
