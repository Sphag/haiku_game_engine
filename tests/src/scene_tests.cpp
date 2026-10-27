// Runtime scene (US-017): loading and saving documents, missing references, failed loads that leave the scene unchanged,
// the prefab library, instances, and overrides. CPU-only; these run on every OS.

#include "engine/scene/scene.hpp"

#include "engine/scene/reflection.hpp"
#include "engine/scene/scene_format.hpp"

#include "engine/core/guid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

using engine::core::Guid;
using engine::scene::ComponentBuilder;
using engine::scene::ComponentCatalog;
using engine::scene::ComponentData;
using engine::scene::EntityRecord;
using engine::scene::FieldValue;
using engine::scene::instance_entity_guid;
using engine::scene::PrefabDocument;
using engine::scene::PrefabLibrary;
using engine::scene::Range;
using engine::scene::Scene;
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
    return catalog;
}

Guid guid_of(std::string_view text) {
    const auto parsed = Guid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

Guid asset(std::uint32_t n) {
    // Distinct, non-nil GUIDs for tests: the last group carries n.
    return guid_of("00000000-0000-4000-8000-" + std::string(12 - std::to_string(n).size(), '0') + std::to_string(n));
}

Transform at(double x, double y, double z) {
    Transform out;
    out.position = glm::dvec3(x, y, z);
    return out;
}

// Prefab "door": the root "frame" and a child "lock" that has a door lock component with default values.
PrefabDocument door_prefab() {
    EntityRecord frame;
    frame.guid = asset(101);
    frame.transform = at(10.0, 0.0, 0.0);  // ignored: an instance takes its root transform from the instance
    EntityRecord lock;
    lock.guid = asset(102);
    lock.parent = asset(101);
    lock.transform = at(0.0, 1.0, 0.0);
    lock.components["game.door_lock"] = ComponentData{};
    PrefabDocument prefab;
    prefab.entities = {frame, lock};
    return prefab;
}

// Round-trips a scene through text, as a save and load does.
SceneDocument through_text(const Scene& scene, const ComponentCatalog& catalog) {
    const auto parsed = engine::scene::parse_scene(engine::scene::serialize_scene(scene.to_document()), catalog);
    REQUIRE(parsed.ok());
    return parsed.value();
}

}  // namespace

TEST_CASE("A scene keeps GUIDs, components, hierarchy, and transforms through a save and load", "[unit]") {
    const auto catalog = make_catalog();
    const PrefabLibrary library(catalog);
    Scene scene(catalog, library);

    const auto root = scene.add_entity(at(1.0e7, 0.0, 0.0));
    REQUIRE(root.ok());
    const auto child = scene.add_entity(at(0.0, 2.0, 0.0), root.value());
    REQUIRE(child.ok());
    REQUIRE(scene.add_component(child.value(), "game.door_lock").ok());
    REQUIRE(scene.set_field(child.value(), "game.door_lock", "strength", 3.0f).ok());

    const std::string saved = engine::scene::serialize_scene(scene.to_document());
    Scene reloaded(catalog, library);
    REQUIRE(reloaded.load(through_text(scene, catalog)).ok());
    CHECK(engine::scene::serialize_scene(reloaded.to_document()) == saved);
    CHECK(reloaded.parent(child.value()) == root.value());
    CHECK(reloaded.transform(root.value())->position == glm::dvec3(1.0e7, 0.0, 0.0));
    const auto strength = reloaded.field(child.value(), "game.door_lock", "strength");
    REQUIRE(strength.has_value());
    CHECK(std::get<float>(*strength) == 3.0f);

    // The loaded hierarchy composes transforms: the child sits 2 m above the far-off root.
    REQUIRE(reloaded.update() == 2);
    const auto* world = reloaded.world_matrix(child.value());
    REQUIRE(world != nullptr);
    CHECK(glm::dvec3((*world)[3]) == glm::dvec3(1.0e7, 2.0, 0.0));
}

TEST_CASE("A reference to a missing asset is kept, reported, and clears once the asset is known", "[unit]") {
    const auto catalog = make_catalog();
    const PrefabLibrary library(catalog);
    const Guid door_key = asset(7);

    Scene scene(catalog, library);
    const auto entity = scene.add_entity(Transform{});
    REQUIRE(entity.ok());
    REQUIRE(scene.add_component(entity.value(), "game.door_lock").ok());
    REQUIRE(scene.set_field(entity.value(), "game.door_lock", "key", door_key).ok());

    const auto missing = scene.missing_references();
    REQUIRE(missing.size() == 1);
    CHECK(missing.front().entity == entity.value());
    CHECK(missing.front().component == "game.door_lock");
    CHECK(missing.front().field == "key");
    CHECK(missing.front().asset == door_key);

    // Loading keeps the reference, so saving again does not drop it.
    Scene unresolved(catalog, library);
    REQUIRE(unresolved.load(through_text(scene, catalog)).ok());
    CHECK(unresolved.missing_references().size() == 1);
    CHECK(std::get<Guid>(*unresolved.field(entity.value(), "game.door_lock", "key")) == door_key);

    Scene resolved(catalog, library);
    REQUIRE(resolved.load(through_text(scene, catalog), std::unordered_set<Guid>{door_key}).ok());
    CHECK(resolved.missing_references().empty());
}

TEST_CASE("A failed load leaves the scene it was loaded into unchanged", "[unit]") {
    const auto catalog = make_catalog();
    const PrefabLibrary library(catalog);
    Scene scene(catalog, library);
    const auto kept = scene.add_entity(at(4.0, 0.0, 0.0));
    REQUIRE(kept.ok());
    const std::string before = engine::scene::serialize_scene(scene.to_document());

    EntityRecord good;
    good.guid = asset(20);
    EntityRecord bad_parent;
    bad_parent.guid = asset(21);
    bad_parent.parent = asset(99);  // names nothing in the document
    EntityRecord cycle_a;
    cycle_a.guid = asset(22);
    cycle_a.parent = asset(23);
    EntityRecord cycle_b;
    cycle_b.guid = asset(23);
    cycle_b.parent = asset(22);
    EntityRecord duplicate;
    duplicate.guid = asset(20);
    EntityRecord bad_transform;
    bad_transform.guid = asset(24);
    bad_transform.transform.rotation = glm::dquat(2.0, 0.0, 0.0, 0.0);

    const std::pair<SceneDocument, SceneErrorCode> cases[] = {
        {SceneDocument{{good, bad_parent}}, SceneErrorCode::MissingParent},
        {SceneDocument{{cycle_a, cycle_b}}, SceneErrorCode::Cycle},
        {SceneDocument{{good, duplicate}}, SceneErrorCode::DuplicateGuid},
        {SceneDocument{{bad_transform}}, SceneErrorCode::InvalidTransform},
    };
    for (const auto& [document, code] : cases) {
        const auto result = scene.load(document);
        REQUIRE_FALSE(result.ok());
        CHECK(result.error().code == code);
        CHECK(engine::scene::serialize_scene(scene.to_document()) == before);
    }
}

TEST_CASE("The prefab library accepts one connected tree and refuses the rest", "[unit]") {
    const auto catalog = make_catalog();
    PrefabLibrary library(catalog);
    REQUIRE(library.add(asset(101), door_prefab()).ok());
    CHECK(library.size() == 1);
    CHECK(library.root_of(asset(101)) == asset(101));

    CHECK(library.add(asset(101), door_prefab()).error().code == SceneErrorCode::DuplicateGuid);
    CHECK(library.add(asset(102), PrefabDocument{}).error().code == SceneErrorCode::InvalidPrefab);

    PrefabDocument two_roots = door_prefab();
    two_roots.entities[1].parent = Guid{};
    CHECK(library.add(asset(103), two_roots).error().code == SceneErrorCode::InvalidPrefab);

    PrefabDocument dangling = door_prefab();
    dangling.entities[1].parent = asset(99);
    CHECK(library.add(asset(104), dangling).error().code == SceneErrorCode::MissingParent);

    // A root with a cycle hanging off it: the two inner entities never reach the root.
    EntityRecord root;
    root.guid = asset(110);
    EntityRecord first;
    first.guid = asset(111);
    first.parent = asset(112);
    EntityRecord second;
    second.guid = asset(112);
    second.parent = asset(111);
    PrefabDocument looped;
    looped.entities = {root, first, second};
    CHECK(library.add(asset(105), looped).error().code == SceneErrorCode::Cycle);

    CHECK(library.size() == 1);
}

TEST_CASE("An instance takes its root transform from the instance and the rest from the prefab", "[unit]") {
    const auto catalog = make_catalog();
    PrefabLibrary library(catalog);
    REQUIRE(library.add(asset(101), door_prefab()).ok());
    Scene scene(catalog, library);

    const auto instance = scene.instantiate(asset(101), at(5.0, 0.0, 0.0));
    REQUIRE(instance.ok());
    CHECK(scene.size() == 2);
    CHECK(scene.prefab_of(instance.value()) == asset(101));
    CHECK(scene.transform(instance.value())->position == glm::dvec3(5.0, 0.0, 0.0));

    const Guid lock = instance_entity_guid(instance.value(), asset(102));
    CHECK(scene.parent(lock) == instance.value());
    REQUIRE(scene.update() == 2);
    const auto* world = scene.world_matrix(lock);
    REQUIRE(world != nullptr);
    CHECK(glm::dvec3((*world)[3]) == glm::dvec3(5.0, 1.0, 0.0));
}

TEST_CASE("Overrides are recorded only where a value differs from the prefab", "[unit]") {
    const auto catalog = make_catalog();
    PrefabLibrary library(catalog);
    REQUIRE(library.add(asset(101), door_prefab()).ok());
    Scene scene(catalog, library);
    const auto instance = scene.instantiate(asset(101), Transform{});
    REQUIRE(instance.ok());
    const Guid lock = instance_entity_guid(instance.value(), asset(102));

    REQUIRE(scene.set_field(lock, "game.door_lock", "locked", false).ok());
    CHECK(scene.is_overridden(lock, "game.door_lock", "locked"));
    CHECK(std::get<bool>(*scene.field(lock, "game.door_lock", "locked")) == false);
    CHECK_FALSE(scene.is_overridden(lock, "game.door_lock", "strength"));

    // The document stores the instance once, with its overrides, and not the lock entity it expands into.
    const SceneDocument document = scene.to_document();
    REQUIRE(document.entities.size() == 1);
    REQUIRE(document.entities.front().prefab.has_value());
    CHECK(document.entities.front().prefab->overrides.size() == 1);
    CHECK(document.entities.front().components.empty());

    // A value equal to the prefab's removes the override.
    REQUIRE(scene.set_field(lock, "game.door_lock", "locked", true).ok());
    CHECK_FALSE(scene.is_overridden(lock, "game.door_lock", "locked"));
    CHECK(scene.to_document().entities.front().prefab->overrides.empty());
}

TEST_CASE("An instance reloads with the same entity GUIDs, values, and overrides", "[unit]") {
    const auto catalog = make_catalog();
    PrefabLibrary library(catalog);
    REQUIRE(library.add(asset(101), door_prefab()).ok());
    Scene scene(catalog, library);
    const auto instance = scene.instantiate(asset(101), at(2.0, 0.0, 0.0));
    REQUIRE(instance.ok());
    const Guid lock = instance_entity_guid(instance.value(), asset(102));
    REQUIRE(scene.set_field(lock, "game.door_lock", "strength", 4.5f).ok());
    const std::string saved = engine::scene::serialize_scene(scene.to_document());

    Scene reloaded(catalog, library);
    REQUIRE(reloaded.load(through_text(scene, catalog)).ok());
    CHECK(engine::scene::serialize_scene(reloaded.to_document()) == saved);
    CHECK(reloaded.contains(lock));
    CHECK(reloaded.is_overridden(lock, "game.door_lock", "strength"));
    CHECK(std::get<float>(*reloaded.field(lock, "game.door_lock", "strength")) == 4.5f);
    CHECK(std::get<bool>(*reloaded.field(lock, "game.door_lock", "locked")) == true);
}

TEST_CASE("Structural edits to an instance and invalid overrides are refused with no change", "[unit]") {
    const auto catalog = make_catalog();
    PrefabLibrary library(catalog);
    REQUIRE(library.add(asset(101), door_prefab()).ok());
    Scene scene(catalog, library);
    const auto instance = scene.instantiate(asset(101), Transform{});
    REQUIRE(instance.ok());
    const Guid lock = instance_entity_guid(instance.value(), asset(102));
    const std::string before = engine::scene::serialize_scene(scene.to_document());

    CHECK(scene.add_component(instance.value(), "game.door_lock").error().code == SceneErrorCode::InstanceEdit);
    CHECK(scene.set_transform(lock, at(1.0, 1.0, 1.0)).error().code == SceneErrorCode::InstanceEdit);
    CHECK(scene.add_entity(Transform{}, instance.value()).error().code == SceneErrorCode::InvalidParent);
    CHECK(scene.set_field(lock, "game.door_lock", "colour", 1.0f).error().code == SceneErrorCode::UnknownField);
    CHECK(scene.set_field(lock, "game.door_lock", "strength", 99.0f).error().code == SceneErrorCode::InvalidValue);
    CHECK(scene.set_field(instance.value(), "game.door_lock", "locked", false).error().code == SceneErrorCode::UnknownComponent);
    CHECK(scene.instantiate(asset(555), Transform{}).error().code == SceneErrorCode::MissingPrefab);
    CHECK(engine::scene::serialize_scene(scene.to_document()) == before);

    // A document whose override names a component the prefab does not have is refused, and so is the whole document.
    SceneDocument bad;
    engine::scene::EntityRecord instance_record;
    instance_record.guid = asset(300);
    instance_record.prefab = engine::scene::PrefabReference{asset(101), {}};
    instance_record.prefab->overrides.push_back(
        engine::scene::PrefabOverride{asset(102), "game.health", "hit_points", FieldValue(std::int32_t{5})});
    bad.entities.push_back(instance_record);
    CHECK(scene.load(bad).error().code == SceneErrorCode::InvalidOverride);
    CHECK(engine::scene::serialize_scene(scene.to_document()) == before);

    // An instance that carries components of its own is refused: its components come from the prefab.
    SceneDocument mixed;
    engine::scene::EntityRecord with_components = instance_record;
    with_components.prefab->overrides.clear();
    with_components.components["game.door_lock"] = ComponentData{};
    mixed.entities.push_back(with_components);
    CHECK(scene.load(mixed).error().code == SceneErrorCode::InvalidPrefab);
    CHECK(engine::scene::serialize_scene(scene.to_document()) == before);

    SceneDocument orphan;
    engine::scene::EntityRecord missing_prefab;
    missing_prefab.guid = asset(301);
    missing_prefab.prefab = engine::scene::PrefabReference{asset(555), {}};
    orphan.entities.push_back(missing_prefab);
    CHECK(scene.load(orphan).error().code == SceneErrorCode::MissingPrefab);
    CHECK(engine::scene::serialize_scene(scene.to_document()) == before);
}
