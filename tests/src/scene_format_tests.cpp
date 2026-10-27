// Scene and prefab files (US-017): round trips, the rejection of unknown or invalid input with an error that names the
// problem, and atomic file IO. The parsing tests are CPU-only and run on every OS. The file tests use a scratch directory.

#include "engine/scene/scene_format.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/scene/reflection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

using engine::core::Guid;
using engine::core::Path;
using engine::scene::ComponentBuilder;
using engine::scene::ComponentCatalog;
using engine::scene::ComponentData;
using engine::scene::EntityRecord;
using engine::scene::FieldValue;
using engine::scene::Range;
using engine::scene::SceneDocument;
using engine::scene::SceneErrorCode;
using engine::scene::Transform;

namespace {

struct DoorLock {
    bool locked = true;
    float strength = 1.0f;
    Guid key{};
    std::uint32_t level = 1;
    std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
};

struct Health {
    std::int32_t hit_points = 100;
};

ComponentCatalog make_catalog() {
    ComponentCatalog catalog;
    ComponentBuilder<DoorLock> door("game.door_lock", 1);
    door.field<&DoorLock::locked>("locked", true);
    door.field<&DoorLock::strength>("strength", 1.0f, Range{0.0, 10.0});
    door.field<&DoorLock::key>("key", Guid{});
    door.field<&DoorLock::level>("level", 1u, Range{1.0, 5.0});
    door.field<&DoorLock::tint>("tint", std::array<float, 3>{1.0f, 1.0f, 1.0f});
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

Guid guid_of(std::string_view text) {
    const auto parsed = Guid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

// A minimal valid scene with one root that has a door lock.
constexpr std::string_view kSceneText = R"({
  "format": "engine.scene",
  "version": 1,
  "entities": [
    {
      "guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
      "parent": null,
      "transform": { "position": [1, 2, 3], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1] },
      "components": {
        "game.door_lock": { "schema": 1, "fields": { "locked": false, "strength": 2.5, "level": 3 } }
      }
    }
  ]
})";

// Parses text that is expected to be valid, for tests that need a document.
SceneDocument parse_ok(std::string_view text, const ComponentCatalog& catalog) {
    auto parsed = engine::scene::parse_scene(text, catalog);
    REQUIRE(parsed.ok());
    return parsed.value();
}

// The rejection: the code must match, and the message must name the problem.
void require_error(std::string_view text, const ComponentCatalog& catalog, SceneErrorCode code, std::string_view mention) {
    const auto parsed = engine::scene::parse_scene(text, catalog);
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().code == code);
    CHECK(parsed.error().message.find(mention) != std::string::npos);
}

// A directory under the system temp location, removed when the test ends.
class ScratchDirectory {
public:
    ScratchDirectory()
        : path_(std::filesystem::temp_directory_path() / ("engine_scene_format_" + Guid::generate().to_string())) {
        std::filesystem::create_directories(path_);
    }
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] Path file(std::string_view name) const {
        const auto joined = Path::from_utf8((path_ / std::string(name)).generic_string());
        REQUIRE(joined.ok());
        return joined.value();
    }
    [[nodiscard]] Path directory() const {
        const auto joined = Path::from_utf8(path_.generic_string());
        REQUIRE(joined.ok());
        return joined.value();
    }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("A scene file round-trips with components, GUIDs, and exact transforms far from the origin", "[unit]") {
    const auto catalog = make_catalog();
    SceneDocument document = parse_ok(kSceneText, catalog);
    REQUIRE(document.entities.size() == 1);
    EntityRecord& entity = document.entities.front();
    entity.transform.position = glm::dvec3(1.0e7 + 0.1, -2.5e6 + 1.0e-9, 3.0);
    entity.components["game.door_lock"].fields["key"] = guid_of("3f2504e0-4f89-41d3-9a0c-0305e82c3302");

    const std::string first = engine::scene::serialize_scene(document);
    const SceneDocument reread = parse_ok(first, catalog);
    CHECK(engine::scene::serialize_scene(reread) == first);

    const EntityRecord& back = reread.entities.front();
    CHECK(back.guid == entity.guid);
    CHECK(back.transform.position == entity.transform.position);
    CHECK(std::get<float>(back.components.at("game.door_lock").fields.at("strength")) == 2.5f);
    CHECK(std::get<bool>(back.components.at("game.door_lock").fields.at("locked")) == false);
    CHECK(std::get<Guid>(back.components.at("game.door_lock").fields.at("key")) == guid_of("3f2504e0-4f89-41d3-9a0c-0305e82c3302"));
}

TEST_CASE("A field missing from a scene file takes its reflected default", "[unit]") {
    const auto catalog = make_catalog();
    const SceneDocument document = parse_ok(kSceneText, catalog);
    const ComponentData& door = document.entities.front().components.at("game.door_lock");
    CHECK(door.fields.size() == 5);
    CHECK(std::get<Guid>(door.fields.at("key")).is_nil());
    CHECK(std::get<std::array<float, 3>>(door.fields.at("tint")) == std::array<float, 3>{1.0f, 1.0f, 1.0f});
    CHECK(std::get<std::uint32_t>(door.fields.at("level")) == 3u);
}

TEST_CASE("A scene file with another format name or version is rejected", "[unit]") {
    const auto catalog = make_catalog();
    require_error(R"({"format": "engine.prefab", "version": 1, "entities": []})", catalog, SceneErrorCode::WrongFormat,
                  "engine.prefab");
    require_error(R"({"format": "engine.scene", "version": 2, "entities": []})", catalog, SceneErrorCode::UnsupportedVersion,
                  "version 2");
    require_error(R"({"format": "engine.scene", "entities": []})", catalog, SceneErrorCode::Malformed, "version");
}

TEST_CASE("Unknown components, schema numbers, and fields are rejected with their names", "[unit]") {
    const auto catalog = make_catalog();
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "components": {"game.teleporter": {"schema": 1, "fields": {}}}}]})",
                  catalog, SceneErrorCode::UnknownComponent, "game.teleporter");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "components": {"game.health": {"schema": 1, "fields": {}}}}]})",
                  catalog, SceneErrorCode::UnsupportedComponentSchema, "game.health");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "components": {"game.door_lock": {"schema": 1, "fields": {"colour": 1}}}}]})",
                  catalog, SceneErrorCode::UnknownField, "colour");
}

TEST_CASE("Wrong JSON types and out-of-range values are rejected", "[unit]") {
    const auto catalog = make_catalog();
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "components": {"game.door_lock": {"schema": 1, "fields": {"locked": 1}}}}]})",
                  catalog, SceneErrorCode::InvalidValue, "locked");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "components": {"game.door_lock": {"schema": 1, "fields": {"level": 9}}}}]})",
                  catalog, SceneErrorCode::InvalidValue, "level");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "components": {"game.door_lock": {"schema": 1, "fields": {"tint": [1, 1]}}}}]})",
                  catalog, SceneErrorCode::InvalidValue, "tint");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "transform": {"rotation": [2, 0, 0, 0]}}]})",
                  catalog, SceneErrorCode::InvalidTransform, "rotation");
}

TEST_CASE("Malformed GUIDs, nil GUIDs, unknown keys, and invalid JSON are rejected", "[unit]") {
    const auto catalog = make_catalog();
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "not-a-guid"}]})", catalog,
                  SceneErrorCode::Malformed, "not-a-guid");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "00000000-0000-0000-0000-000000000000"}]})",
                  catalog, SceneErrorCode::NilGuid, "nil GUID");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "colour": "red"}]})",
                  catalog, SceneErrorCode::Malformed, "colour");
    require_error(R"({"format": "engine.scene", "version": 1, "entities": [{"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301",
        "parent": "00000000-0000-0000-0000-000000000000"}]})",
                  catalog, SceneErrorCode::Malformed, "parent");
    require_error("{ not json", catalog, SceneErrorCode::Malformed, "valid JSON");
}

TEST_CASE("A prefab file rejects instances and round-trips without them", "[unit]") {
    const auto catalog = make_catalog();
    const std::string_view with_instance = R"({"format": "engine.prefab", "version": 1, "entities": [
        {"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301", "parent": null,
         "prefab": {"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3302"}}]})";
    const auto rejected = engine::scene::parse_prefab(with_instance, catalog);
    REQUIRE_FALSE(rejected.ok());
    CHECK(rejected.error().code == SceneErrorCode::Malformed);

    const std::string_view plain = R"({"format": "engine.prefab", "version": 1, "entities": [
        {"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301", "parent": null, "components": {"game.health": {"schema": 2, "fields": {}}}}]})";
    const auto parsed = engine::scene::parse_prefab(plain, catalog);
    REQUIRE(parsed.ok());
    const std::string text = engine::scene::serialize_prefab(parsed.value());
    const auto again = engine::scene::parse_prefab(text, catalog);
    if (!again.ok()) {
        FAIL("re-parse failed: " << again.error().message << "\n" << text);
    }
    CHECK(engine::scene::serialize_prefab(again.value()) == text);
}

TEST_CASE("An instance's overrides survive a round trip without the prefab's values being copied", "[unit]") {
    const auto catalog = make_catalog();
    const std::string_view text = R"({"format": "engine.scene", "version": 1, "entities": [
        {"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3301", "parent": null,
         "prefab": {"guid": "3f2504e0-4f89-41d3-9a0c-0305e82c3302",
                    "overrides": [{"entity": "3f2504e0-4f89-41d3-9a0c-0305e82c3303", "component": "game.door_lock",
                                   "field": "locked", "value": false}]}}]})";
    const SceneDocument document = parse_ok(text, catalog);
    REQUIRE(document.entities.front().prefab.has_value());
    CHECK(document.entities.front().prefab->overrides.size() == 1);
    CHECK(document.entities.front().components.empty());

    const std::string written = engine::scene::serialize_scene(document);
    CHECK(written.find("\"fields\"") == std::string::npos);
    CHECK(engine::scene::serialize_scene(parse_ok(written, catalog)) == written);
}

TEST_CASE("Scene and prefab files save and load through the file system", "[integration]") {
    const auto catalog = make_catalog();
    const ScratchDirectory scratch;
    const SceneDocument document = parse_ok(kSceneText, catalog);

    const Path file = scratch.file("level.scene.json");
    const auto saved = engine::scene::save_scene(file, document);
    REQUIRE(saved.ok());
    CHECK_FALSE(engine::core::path_exists(scratch.file("level.scene.json.tmp")));

    const auto loaded = engine::scene::load_scene(file, catalog);
    REQUIRE(loaded.ok());
    CHECK(engine::scene::serialize_scene(loaded.value()) == engine::scene::serialize_scene(document));

    const auto missing = engine::scene::load_scene(scratch.file("absent.scene.json"), catalog);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == SceneErrorCode::Io);
}

TEST_CASE("A failed save leaves the previous file untouched", "[integration]") {
    const auto catalog = make_catalog();
    const ScratchDirectory scratch;
    const SceneDocument document = parse_ok(kSceneText, catalog);
    const Path file = scratch.file("level.scene.json");
    REQUIRE(engine::scene::save_scene(file, document).ok());
    const std::string before = engine::scene::serialize_scene(document);

    // The parent directory does not exist, so the write fails before any file is replaced.
    const Path orphan = Path::from_utf8((scratch.directory().str() + "/missing/level.scene.json")).value();
    const auto failed = engine::scene::save_scene(orphan, document);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == SceneErrorCode::Io);

    const auto reloaded = engine::scene::load_scene(file, catalog);
    REQUIRE(reloaded.ok());
    CHECK(engine::scene::serialize_scene(reloaded.value()) == before);
}
