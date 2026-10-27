// Entity handles and world lifetime (US-012). CPU-only; these run on every operating system.

#include "engine/ecs/entity.hpp"
#include "engine/ecs/world.hpp"

#include "engine/core/guid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

using engine::core::Guid;
using engine::ecs::EntityHandle;
using engine::ecs::World;
using engine::ecs::WorldError;

namespace {

// Creates a persistent-id entity and fails the test if the id is rejected.
EntityHandle must_create(World& world, const Guid& guid) {
    auto result = world.create(guid);
    REQUIRE(result.ok());
    return result.value();
}

}  // namespace

TEST_CASE("A default handle is invalid and never alive in any world", "[unit]") {
    const EntityHandle none;
    World world;

    CHECK_FALSE(none.valid());
    CHECK_FALSE(world.alive(none));
    CHECK_FALSE(world.destroy(none));
}

TEST_CASE("A created entity is alive until it is destroyed", "[unit]") {
    World world;
    const EntityHandle entity = world.create();

    CHECK(entity.valid());
    CHECK(world.alive(entity));
    CHECK(world.size() == 1);

    CHECK(world.destroy(entity));
    CHECK_FALSE(world.alive(entity));
    CHECK(world.size() == 0);
}

TEST_CASE("Destroying an entity twice fails the second time and leaves the count unchanged", "[unit]") {
    World world;
    const EntityHandle entity = world.create();
    const EntityHandle other = world.create();

    REQUIRE(world.destroy(entity));
    CHECK_FALSE(world.destroy(entity));
    CHECK_FALSE(world.destroy(entity));
    CHECK(world.size() == 1);
    CHECK(world.alive(other));
}

TEST_CASE("A stale handle does not revive when its slot is reused", "[unit]") {
    World world;
    const EntityHandle first = world.create();
    REQUIRE(world.destroy(first));

    const EntityHandle second = world.create();

    // The slot is reused, so the index matches, but the generation has moved on.
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    CHECK(world.alive(second));
    CHECK_FALSE(world.alive(first));
    CHECK_FALSE(world.destroy(first));
    CHECK(world.alive(second));
}

TEST_CASE("Repeated create and destroy cycles keep stale handles dead", "[unit]") {
    World world;
    std::vector<EntityHandle> stale;

    for (int cycle = 0; cycle < 500; ++cycle) {
        const EntityHandle entity = world.create();
        REQUIRE(world.alive(entity));
        REQUIRE(world.destroy(entity));
        stale.push_back(entity);
    }

    CHECK(world.size() == 0);
    for (const auto& handle : stale) {
        CHECK_FALSE(world.alive(handle));
        CHECK_FALSE(world.destroy(handle));
    }
}

TEST_CASE("Bulk creation yields distinct live handles and survives mass destruction", "[unit]") {
    constexpr std::size_t kCount = 20000;
    World world;

    std::vector<EntityHandle> entities;
    entities.reserve(kCount);
    std::unordered_set<std::uint64_t> keys;
    for (std::size_t i = 0; i < kCount; ++i) {
        const EntityHandle entity = world.create();
        entities.push_back(entity);
        keys.insert((static_cast<std::uint64_t>(entity.index) << 32) | entity.generation);
    }

    CHECK(keys.size() == kCount);
    CHECK(world.size() == kCount);
    for (const auto& entity : entities) {
        REQUIRE(world.alive(entity));
    }

    // Destroy every other entity, then refill. The old handles stay dead and the live count stays exact.
    std::vector<EntityHandle> removed;
    for (std::size_t i = 0; i < kCount; i += 2) {
        REQUIRE(world.destroy(entities[i]));
        removed.push_back(entities[i]);
    }
    CHECK(world.size() == kCount / 2);

    for (std::size_t i = 0; i < kCount / 2; ++i) {
        CHECK(world.alive(world.create()));
    }
    CHECK(world.size() == kCount);
    for (const auto& handle : removed) {
        CHECK_FALSE(world.alive(handle));
    }
    for (std::size_t i = 1; i < kCount; i += 2) {
        CHECK(world.alive(entities[i]));
    }
}

TEST_CASE("A handle from another world is rejected even when index and generation match", "[unit]") {
    World first;
    World second;
    const EntityHandle mine = second.create();
    const EntityHandle theirs = first.create();

    // Same slot and generation, different owner.
    REQUIRE(theirs.index == mine.index);
    REQUIRE(theirs.generation == mine.generation);
    REQUIRE(theirs.world != mine.world);

    CHECK_FALSE(first.alive(mine));
    CHECK_FALSE(second.alive(theirs));
    CHECK_FALSE(first.destroy(mine));
    CHECK_FALSE(second.destroy(theirs));

    // A rejected foreign destroy must not touch the entity it names.
    CHECK(first.alive(theirs));
    CHECK(second.alive(mine));
    CHECK(first.size() == 1);
    CHECK(second.size() == 1);
}

TEST_CASE("Each world has its own id and handles carry it", "[unit]") {
    World first;
    World second;

    CHECK(first.id() != 0);
    CHECK(second.id() != 0);
    CHECK(first.id() != second.id());
    CHECK(first.create().world == first.id());
    CHECK(second.create().world == second.id());
}

TEST_CASE("Persistent GUIDs are independent of entity indices", "[unit]") {
    World world;
    const Guid node = Guid::generate();
    const Guid other = Guid::generate();

    const EntityHandle plain = world.create();
    const EntityHandle tagged = must_create(world, node);

    // The GUID is not the index, and an index is not a GUID: each only names the entity it was issued for.
    CHECK(world.persistent_id(tagged) == node);
    CHECK_FALSE(world.persistent_id(plain).has_value());
    CHECK(world.find(node) == tagged);
    CHECK_FALSE(world.find(other).has_value());
}

TEST_CASE("A persistent id is unique within a world and rejects nil", "[unit]") {
    World world;
    const Guid node = Guid::generate();
    const EntityHandle entity = must_create(world, node);

    const auto duplicate = world.create(node);
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error() == WorldError::DuplicateGuid);
    CHECK(world.size() == 1);

    const auto nil = world.create(Guid{});
    REQUIRE_FALSE(nil.ok());
    CHECK(nil.error() == WorldError::NilGuid);
    CHECK(world.size() == 1);
    CHECK(world.alive(entity));
}

TEST_CASE("Destroying a tagged entity releases its GUID for a new entity", "[unit]") {
    World world;
    const Guid node = Guid::generate();
    const EntityHandle old_entity = must_create(world, node);

    REQUIRE(world.destroy(old_entity));
    CHECK_FALSE(world.find(node).has_value());
    CHECK_FALSE(world.persistent_id(old_entity).has_value());

    const EntityHandle reloaded = must_create(world, node);
    CHECK(world.alive(reloaded));
    CHECK_FALSE(world.alive(old_entity));
    CHECK(world.find(node) == reloaded);
}

TEST_CASE("The same GUID in two worlds names two independent entities", "[unit]") {
    World first;
    World second;
    const Guid scene_node = Guid::generate();

    // Burn a slot in the second world so its entity gets a later generation than the first world's entity.
    const EntityHandle filler = second.create();
    REQUIRE(second.destroy(filler));

    const EntityHandle in_first = must_create(first, scene_node);
    const EntityHandle in_second = must_create(second, scene_node);

    CHECK(in_first != in_second);
    CHECK(first.find(scene_node) == in_first);
    CHECK(second.find(scene_node) == in_second);
    CHECK_FALSE(first.alive(in_second));
    CHECK_FALSE(second.alive(in_first));

    REQUIRE(first.destroy(in_first));
    CHECK(second.alive(in_second));
    CHECK(second.find(scene_node) == in_second);
}

TEST_CASE("Bulk tagged creation finds every GUID and releases them all on destruction", "[unit]") {
    constexpr std::size_t kCount = 5000;
    World world;

    std::vector<Guid> guids;
    std::vector<EntityHandle> entities;
    guids.reserve(kCount);
    entities.reserve(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        guids.push_back(Guid::generate());
        entities.push_back(must_create(world, guids.back()));
    }

    CHECK(world.size() == kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        CHECK(world.find(guids[i]) == entities[i]);
    }

    for (const auto& handle : entities) {
        REQUIRE(world.destroy(handle));
    }
    CHECK(world.size() == 0);
    for (const auto& guid : guids) {
        CHECK_FALSE(world.find(guid).has_value());
    }
}

TEST_CASE("A moved world keeps its entities and the source becomes empty", "[unit]") {
    World source;
    const Guid node = Guid::generate();
    const EntityHandle entity = must_create(source, node);
    const std::uint32_t original_id = source.id();

    World target = std::move(source);

    CHECK(target.id() == original_id);
    CHECK(target.alive(entity));
    CHECK(target.find(node) == entity);
    CHECK(target.size() == 1);

    CHECK(source.size() == 0);  // NOLINT(bugprone-use-after-move)
    CHECK_FALSE(source.alive(entity));
    CHECK_FALSE(source.find(node).has_value());
}
