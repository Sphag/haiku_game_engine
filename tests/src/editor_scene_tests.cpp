// The scene editor (US-062): the hierarchy's create, duplicate, remove, reparent, and search; the selection; reflected components and
// field validation; the unsaved state; saving and opening, including missing assets, malformed files, and missing prefabs. The undo
// and redo history (US-064) is tested here too: each command, gesture merging, redo clearing, dirty state, and a full rollback.
// No window or graphics device is used.

#include "editor_fixture.hpp"

#include "engine/editor/scene_editor.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/scene/reflection.hpp"
#include "engine/scene/scene_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using engine::core::Guid;
using engine::core::LogLevel;
using engine::editor::Console;
using engine::editor::HierarchyRow;
using engine::editor::SceneEditor;
using engine::scene::ComponentBuilder;
using engine::scene::FieldValue;
using engine::scene::Range;
using engine::scene::SceneErrorCode;
using engine::tests::editor_fixture::Scratch;
using engine::tests::editor_fixture::write_file;

namespace {

// A component with one field of each reflected kind that the inspector has a widget for.
struct Light {
    float intensity = 1.0f;
    std::uint32_t mode = 0;
    bool lit = true;
    std::int32_t layer = 0;
    std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
    Guid mask;
};

constexpr std::string_view kLightType = "game.light";
constexpr std::string_view kSceneGuid = "3f2504e0-4f89-41d3-9a0c-0305e82c3301";
constexpr std::string_view kMaskGuid = "6b1f2a4e-0c3d-4e5f-8a9b-1c2d3e4f5a60";
constexpr std::string_view kPrefabGuid = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";

// Registers game.light with ranges on intensity, so the validation path is exercised.
void register_light(SceneEditor& editor) {
    ComponentBuilder<Light> builder(kLightType, 1);
    builder.field<&Light::intensity>("intensity", 1.0f, Range{0.0, 10.0})
        .field<&Light::mode>("mode", 0u)
        .field<&Light::lit>("lit", true)
        .field<&Light::layer>("layer", 0)
        .field<&Light::tint>("tint", {1.0f, 1.0f, 1.0f})
        .field<&Light::mask>("mask", Guid{});
    auto descriptor = builder.build();
    REQUIRE(descriptor.ok());
    REQUIRE(editor.catalog().add(std::move(descriptor).value()).ok());
}

// True when the console holds an entry at the given level whose message contains `text`.
bool console_has(const Console& console, LogLevel level, std::string_view text) {
    for (const auto& entry : console.snapshot()) {
        if (entry.level == level && entry.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The mask GUID as a value. The constant is a literal, so a parse failure is a test bug.
Guid mask_guid() {
    auto parsed = Guid::parse(kMaskGuid);
    REQUIRE(parsed.ok());
    return parsed.value();
}

std::vector<Guid> guids_of(const std::vector<HierarchyRow>& rows) {
    std::vector<Guid> guids;
    for (const HierarchyRow& row : rows) {
        guids.push_back(row.guid);
    }
    return guids;
}

std::size_t depth_of(const std::vector<HierarchyRow>& rows, const Guid& guid) {
    for (const HierarchyRow& row : rows) {
        if (row.guid == guid) {
            return row.depth;
        }
    }
    return 999;
}

}  // namespace

TEST_CASE("Entities are created as a tree, listed in order, and the new entity is selected", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);

    const Guid root = editor.create_entity().value();
    const Guid child = editor.create_entity(root).value();
    const Guid grandchild = editor.create_entity(child).value();

    const auto rows = editor.hierarchy("");
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].guid == root);
    CHECK(depth_of(rows, child) == 1);
    CHECK(depth_of(rows, grandchild) == 2);
    CHECK(editor.selected() == grandchild);
    CHECK(editor.dirty());
    CHECK(console_has(*console, LogLevel::Info, "created entity"));
}

TEST_CASE("Removing an entity removes the subtree below it and clears a selection among them", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid root = editor.create_entity().value();
    const Guid child = editor.create_entity(root).value();
    static_cast<void>(editor.create_entity(child));
    const Guid other = editor.create_entity().value();

    editor.select(child);
    REQUIRE(editor.remove(child).ok());
    CHECK(editor.hierarchy("").size() == 2);
    CHECK_FALSE(editor.scene().contains(child));
    CHECK_FALSE(editor.selected().has_value());
    CHECK(editor.scene().contains(root));
    CHECK(editor.scene().contains(other));
    CHECK_FALSE(editor.remove(child).ok());
}

TEST_CASE("Duplicate copies an entity and its subtree under the same parent with new GUIDs", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    const Guid root = editor.create_entity().value();
    const Guid child = editor.create_entity(root).value();
    REQUIRE(editor.add_component(child, kLightType).ok());

    const Guid copy = editor.duplicate(root).value();
    CHECK(copy != root);
    CHECK(editor.scene().parent(copy).is_nil());
    CHECK(editor.selected() == copy);
    const auto rows = editor.hierarchy("");
    REQUIRE(rows.size() == 4);
    CHECK(depth_of(rows, copy) == 0);

    // The copy of the child sits under the copy of the root and keeps its component.
    const auto copied_light = std::any_of(rows.begin(), rows.end(), [&](const HierarchyRow& row) {
        return row.depth == 1 && editor.scene().parent(row.guid) == copy && !editor.scene().component_types(row.guid).empty();
    });
    CHECK(copied_light);
}

TEST_CASE("Reparenting under a descendant is refused and the scene stays as it was", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid parent = editor.create_entity().value();
    const Guid child = editor.create_entity(parent).value();
    const Guid grandchild = editor.create_entity(child).value();

    // parent is an ancestor of grandchild, so it cannot move under it.
    CHECK_FALSE(editor.reparent(parent, grandchild).ok());
    CHECK(editor.scene().parent(parent).is_nil());
    CHECK(editor.scene().parent(child) == parent);
    CHECK(console_has(*console, LogLevel::Error, "cannot"));

    // The grandchild can become a root, and then parent can move under it.
    REQUIRE(editor.reparent(grandchild, Guid{}).ok());
    CHECK(editor.scene().parent(grandchild).is_nil());
    REQUIRE(editor.reparent(parent, grandchild).ok());
    CHECK(editor.scene().parent(parent) == grandchild);
}

TEST_CASE("Components come from the reflected catalog and their fields are validated", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    const Guid entity = editor.create_entity().value();

    REQUIRE(editor.add_component(entity, kLightType).ok());
    CHECK(editor.scene().component_types(entity) == std::vector<std::string>{std::string(kLightType)});
    CHECK_FALSE(editor.add_component(entity, "game.unknown").ok());

    REQUIRE(editor.set_field(entity, kLightType, "intensity", FieldValue(std::in_place_type<float>, 5.0f)).ok());
    CHECK(std::get<float>(*editor.scene().field(entity, kLightType, "intensity")) == 5.0f);

    // A value outside the range, a non-finite value, and a value of the wrong kind are refused, and the field keeps its value.
    CHECK_FALSE(editor.set_field(entity, kLightType, "intensity", FieldValue(std::in_place_type<float>, 99.0f)).ok());
    CHECK_FALSE(editor.set_field(entity, kLightType, "intensity", FieldValue(std::in_place_type<float>, std::nanf(""))).ok());
    CHECK_FALSE(editor.set_field(entity, kLightType, "lit", FieldValue(std::in_place_type<float>, 1.0f)).ok());
    CHECK(std::get<float>(*editor.scene().field(entity, kLightType, "intensity")) == 5.0f);
    CHECK(console_has(*console, LogLevel::Warning, "rejected"));

    REQUIRE(editor.remove_component(entity, kLightType).ok());
    CHECK(editor.scene().component_types(entity).empty());
    CHECK_FALSE(editor.remove_component(entity, kLightType).ok());
}

TEST_CASE("The unsaved state follows the scene's text, so a change undone is clean again", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    CHECK_FALSE(editor.dirty());

    const Guid entity = editor.create_entity().value();
    REQUIRE(editor.add_component(entity, kLightType).ok());
    REQUIRE(editor.save_as(scratch.path.join("scene.json").value()).ok());
    CHECK_FALSE(editor.dirty());

    REQUIRE(editor.set_field(entity, kLightType, "intensity", FieldValue(std::in_place_type<float>, 5.0f)).ok());
    CHECK(editor.dirty());
    REQUIRE(editor.set_field(entity, kLightType, "intensity", FieldValue(std::in_place_type<float>, 1.0f)).ok());
    CHECK_FALSE(editor.dirty());
}

TEST_CASE("A scene saves and opens again with the same entities, values, and file", "[unit][editor]") {
    Scratch scratch;
    const auto file = scratch.path.join("level.scene.json").value();
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    const Guid root = editor.create_entity().value();
    const Guid child = editor.create_entity(root).value();
    REQUIRE(editor.add_component(child, kLightType).ok());
    REQUIRE(editor.set_field(child, kLightType, "layer", FieldValue(std::in_place_type<std::int32_t>, 3)).ok());
    REQUIRE(editor.save_as(file).ok());
    REQUIRE(editor.path().has_value());
    CHECK(editor.path()->str() == file.str());

    SceneEditor reopened(std::make_shared<Console>());
    register_light(reopened);
    REQUIRE(reopened.open(file).ok());
    CHECK_FALSE(reopened.dirty());
    CHECK(reopened.hierarchy("").size() == 2);
    CHECK(reopened.scene().parent(child) == root);
    CHECK(std::get<std::int32_t>(*reopened.scene().field(child, kLightType, "layer")) == 3);
}

TEST_CASE("Saving a scene that has no file asks for Save As, and a failed open keeps the scene", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid kept = editor.create_entity().value();

    CHECK_FALSE(editor.save().ok());
    CHECK_FALSE(editor.open(scratch.path.join("missing.scene.json").value()).ok());
    CHECK(console_has(*console, LogLevel::Error, "cannot open scene"));
    CHECK(editor.scene().contains(kept));
    CHECK(editor.hierarchy("").size() == 1);
}

TEST_CASE("A malformed scene file is refused without changing the scene", "[unit][editor]") {
    Scratch scratch;
    write_file(scratch.path, "broken.scene.json", "{ \"format\": \"engine.scene\", \"version\": 1, ");
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid kept = editor.create_entity().value();

    CHECK_FALSE(editor.open(scratch.path.join("broken.scene.json").value()).ok());
    CHECK(editor.scene().contains(kept));
    CHECK(console_has(*console, LogLevel::Error, "broken.scene.json"));
}

TEST_CASE("A reference to a missing asset opens, shows its GUID, and clears once the asset is known", "[unit][editor]") {
    Scratch scratch;
    const std::string text = std::string("{\"format\":\"engine.scene\",\"version\":1,\"entities\":[{\"guid\":\"") + std::string(kSceneGuid) +
                             "\",\"parent\":null,\"transform\":{\"position\":[0,0,0],\"rotation\":[1,0,0,0],\"scale\":[1,1,1]},"
                             "\"components\":{\"game.light\":{\"schema\":1,\"fields\":{\"mask\":\"" + std::string(kMaskGuid) + "\"}}}}]}";
    write_file(scratch.path, "masked.scene.json", text);
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);

    REQUIRE(editor.open(scratch.path.join("masked.scene.json").value()).ok());
    const auto missing = editor.missing_references();
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].asset == Guid::parse(kMaskGuid).value());
    CHECK_FALSE(editor.is_known_asset(missing[0].asset));
    CHECK(console_has(*console, LogLevel::Warning, std::string(kMaskGuid)));

    editor.set_known_assets({Guid::parse(kMaskGuid).value()});
    CHECK(editor.missing_references().empty());
    CHECK(editor.is_known_asset(Guid::parse(kMaskGuid).value()));
    CHECK_FALSE(editor.dirty());
}

TEST_CASE("A scene that names a prefab which is not in the library fails with the prefab's GUID", "[unit][editor]") {
    Scratch scratch;
    const std::string text = std::string("{\"format\":\"engine.scene\",\"version\":1,\"entities\":[{\"guid\":\"") + std::string(kSceneGuid) +
                             "\",\"parent\":null,\"transform\":{\"position\":[0,0,0],\"rotation\":[1,0,0,0],\"scale\":[1,1,1]},"
                             "\"prefab\":{\"guid\":\"" + std::string(kPrefabGuid) + "\",\"overrides\":[]}}]}";
    write_file(scratch.path, "instance.scene.json", text);
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);

    const auto opened = editor.open(scratch.path.join("instance.scene.json").value());
    REQUIRE_FALSE(opened.ok());
    CHECK(opened.error().code == SceneErrorCode::MissingPrefab);
    CHECK(opened.error().message.find(std::string(kPrefabGuid)) != std::string::npos);
    CHECK(editor.hierarchy("").empty());
}

TEST_CASE("The hierarchy filter matches GUID text and component types, ignoring case", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    const Guid lit = editor.create_entity().value();
    const Guid plain = editor.create_entity().value();
    REQUIRE(editor.add_component(lit, kLightType).ok());

    const auto by_component = guids_of(editor.hierarchy("GAME.LIGHT"));
    REQUIRE(by_component.size() == 1);
    CHECK(by_component[0] == lit);

    const auto by_guid = guids_of(editor.hierarchy(plain.to_string().substr(0, 8)));
    REQUIRE(by_guid.size() == 1);
    CHECK(by_guid[0] == plain);
    CHECK(editor.hierarchy("no such thing").empty());
}

TEST_CASE("Undo and redo restore created and removed entities with their GUIDs", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid root = editor.create_entity().value();
    const Guid child = editor.create_entity(root).value();
    CHECK(editor.undo_label() == "create entity");

    REQUIRE(editor.undo());
    CHECK_FALSE(editor.scene().contains(child));
    CHECK(editor.scene().contains(root));
    REQUIRE(editor.undo());
    CHECK_FALSE(editor.scene().contains(root));
    CHECK_FALSE(editor.can_undo());
    CHECK_FALSE(editor.undo());
    CHECK(editor.can_redo());

    REQUIRE(editor.redo());
    REQUIRE(editor.redo());
    CHECK(editor.scene().contains(root));
    CHECK(editor.scene().contains(child));
    CHECK(editor.scene().parent(child) == root);
    CHECK_FALSE(editor.can_redo());

    // Removing a subtree is one command: its undo brings back every entity in it under the same GUIDs.
    REQUIRE(editor.remove(root).ok());
    CHECK(editor.hierarchy("").empty());
    REQUIRE(editor.undo());
    CHECK(editor.scene().parent(child) == root);
    CHECK(editor.hierarchy("").size() == 2);
}

TEST_CASE("Undo reverts a reparent and a duplicate with their hierarchy", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid a = editor.create_entity().value();
    const Guid b = editor.create_entity().value();
    const engine::scene::SceneDocument before_move = editor.scene().to_document();

    REQUIRE(editor.reparent(b, a).ok());
    CHECK(editor.scene().parent(b) == a);
    CHECK(editor.undo_label() == "reparent");
    REQUIRE(editor.undo());
    CHECK(editor.scene().parent(b).is_nil());
    CHECK(engine::scene::serialize_scene(editor.scene().to_document()) == engine::scene::serialize_scene(before_move));

    const Guid copy = editor.duplicate(a).value();
    CHECK(editor.hierarchy("").size() == 3);
    REQUIRE(editor.undo());
    CHECK_FALSE(editor.scene().contains(copy));
    CHECK(editor.scene().contains(a));
}

TEST_CASE("Undo and redo restore component edits and Guid references", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    editor.set_known_assets({mask_guid()});
    const Guid entity = editor.create_entity().value();
    REQUIRE(editor.add_component(entity, kLightType).ok());
    REQUIRE(editor.undo_label() == "add " + std::string(kLightType));
    REQUIRE(editor.set_field(entity, kLightType, "intensity", FieldValue(std::in_place_type<float>, 5.0f)).ok());
    REQUIRE(editor.set_field(entity, kLightType, "mask", FieldValue(std::in_place_type<Guid>, mask_guid())).ok());

    REQUIRE(editor.undo());
    CHECK(std::get<Guid>(*editor.scene().field(entity, kLightType, "mask")).is_nil());
    CHECK(std::get<float>(*editor.scene().field(entity, kLightType, "intensity")) == 5.0f);
    REQUIRE(editor.undo());
    CHECK(std::get<float>(*editor.scene().field(entity, kLightType, "intensity")) == 1.0f);
    REQUIRE(editor.undo());
    const std::vector<std::string> types = editor.scene().component_types(entity);
    CHECK(std::find(types.begin(), types.end(), std::string(kLightType)) == types.end());
    REQUIRE(editor.redo());
    REQUIRE(editor.redo());
    REQUIRE(editor.redo());
    CHECK(std::get<float>(*editor.scene().field(entity, kLightType, "intensity")) == 5.0f);
    CHECK(std::get<Guid>(*editor.scene().field(entity, kLightType, "mask")) == mask_guid());
}

TEST_CASE("A gizmo drag merges into one command, and a later drag starts a new one", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid entity = editor.create_entity().value();
    const engine::scene::Transform start = *editor.scene().transform(entity);

    editor.begin_gesture();
    for (int frame = 1; frame <= 5; ++frame) {
        engine::scene::Transform moved = start;
        moved.position.x = 0.5 * frame;
        REQUIRE(editor.set_transform(entity, moved).ok());
    }
    editor.end_gesture();
    CHECK(editor.undo_label().substr(0, 5) == "move ");
    CHECK(editor.scene().transform(entity)->position.x == 2.5);

    // One undo returns to the press, not to the drag's last intermediate value.
    REQUIRE(editor.undo());
    CHECK(editor.scene().transform(entity)->position == start.position);
    // The create entity command is the only one left, so the drag was exactly one step.
    CHECK(editor.undo_label() == "create entity");

    // A second drag on the same entity after the gesture ended is a new command, not a merge into the first.
    REQUIRE(editor.redo());
    editor.begin_gesture();
    engine::scene::Transform second = start;
    second.position.x = 9.0;
    REQUIRE(editor.set_transform(entity, second).ok());
    editor.end_gesture();
    REQUIRE(editor.set_transform(entity, start).ok());  // outside a gesture: its own command
    REQUIRE(editor.undo());
    CHECK(editor.scene().transform(entity)->position.x == 9.0);
    // The second command's before-state is the first drag's end value, which the redo restored.
    REQUIRE(editor.undo());
    CHECK(editor.scene().transform(entity)->position.x == 2.5);
}

TEST_CASE("A new command clears the redo steps", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    const Guid a = editor.create_entity().value();
    REQUIRE(editor.undo());
    CHECK(editor.can_redo());
    static_cast<void>(editor.create_entity());
    CHECK_FALSE(editor.can_redo());
    CHECK_FALSE(editor.redo());
    CHECK_FALSE(editor.scene().contains(a));
}

TEST_CASE("Undo and redo move the dirty state with the saved text", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    auto file = scratch.path.join("history.scene");
    REQUIRE(file.ok());
    static_cast<void>(editor.create_entity());
    REQUIRE(editor.save_as(file.value()).ok());
    CHECK_FALSE(editor.dirty());

    const Guid extra = editor.create_entity().value();
    CHECK(editor.dirty());
    REQUIRE(editor.undo());
    CHECK_FALSE(editor.dirty());
    REQUIRE(editor.redo());
    CHECK(editor.dirty());
    CHECK(editor.scene().contains(extra));
}

TEST_CASE("An edit-and-full-rollback scenario restores the original serialized document", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    editor.set_known_assets({mask_guid()});

    // The original scene: two entities with a component, saved so the rollback can be checked against the saved text as well.
    const Guid root = editor.create_entity().value();
    const Guid child = editor.create_entity(root).value();
    REQUIRE(editor.add_component(child, kLightType).ok());
    REQUIRE(editor.set_field(child, kLightType, "intensity", FieldValue(std::in_place_type<float>, 2.0f)).ok());
    const std::string original = engine::scene::serialize_scene(editor.scene().to_document());
    auto file = scratch.path.join("rollback.scene");
    REQUIRE(file.ok());
    REQUIRE(editor.save_as(file.value()).ok());

    // A mix of every kind of command, with a drag in the middle.
    const Guid extra = editor.create_entity(child).value();
    REQUIRE(editor.reparent(child, Guid{}).ok());
    REQUIRE(editor.set_field(child, kLightType, "mask", FieldValue(std::in_place_type<Guid>, mask_guid())).ok());
    REQUIRE(editor.add_component(extra, kLightType).ok());
    REQUIRE(editor.remove_component(child, kLightType).ok());
    const Guid copy = editor.duplicate(root).value();
    editor.begin_gesture();
    const engine::scene::Transform start = *editor.scene().transform(root);
    for (int frame = 1; frame <= 3; ++frame) {
        engine::scene::Transform moved = start;
        moved.scale = glm::dvec3(1.0 + frame);
        REQUIRE(editor.set_transform(root, moved).ok());
    }
    editor.end_gesture();
    REQUIRE(editor.remove(copy).ok());
    const std::string edited = engine::scene::serialize_scene(editor.scene().to_document());
    CHECK(edited != original);
    CHECK(editor.dirty());

    // The scenario is eight commands (the gesture is one). Undoing them restores the original document byte for byte.
    std::size_t undone = 0;
    for (; undone < 8; ++undone) {
        REQUIRE(editor.undo());
    }
    CHECK(engine::scene::serialize_scene(editor.scene().to_document()) == original);
    CHECK_FALSE(editor.dirty());
    CHECK(editor.can_redo());

    // Rolling back the rest empties the scene; rolling forward again returns the edited document exactly.
    while (editor.undo()) {
        ++undone;
    }
    CHECK(undone == 12);
    CHECK(editor.hierarchy("").empty());
    CHECK(engine::scene::serialize_scene(editor.scene().to_document()) == engine::scene::serialize_scene(engine::scene::SceneDocument{}));
    while (editor.redo()) {
        ++undone;
    }
    CHECK(undone == 24);
    CHECK(engine::scene::serialize_scene(editor.scene().to_document()) == edited);
}

TEST_CASE("A rejected edit records nothing, and opening a scene clears the history", "[unit][editor]") {
    Scratch scratch;
    auto console = std::make_shared<Console>();
    SceneEditor editor(console);
    register_light(editor);
    const Guid entity = editor.create_entity().value();
    const std::string label = std::string(editor.undo_label());

    engine::scene::Transform broken;
    broken.scale = glm::dvec3(0.0, 1.0, 1.0);
    broken.position = glm::dvec3(std::nan(""), 0.0, 0.0);
    CHECK_FALSE(editor.set_transform(entity, broken).ok());
    CHECK(editor.undo_label() == label);
    CHECK_FALSE(editor.set_field(entity, kLightType, "intensity", FieldValue(std::in_place_type<float>, 1.0f)).ok());
    CHECK(editor.undo_label() == label);

    auto file = scratch.path.join("cleared.scene");
    REQUIRE(file.ok());
    REQUIRE(editor.save_as(file.value()).ok());
    REQUIRE(editor.open(file.value()).ok());
    CHECK_FALSE(editor.can_undo());
    CHECK_FALSE(editor.can_redo());
    CHECK_FALSE(editor.dirty());
}
