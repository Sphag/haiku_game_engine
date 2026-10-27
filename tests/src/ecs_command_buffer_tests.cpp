// Deferred structural changes (US-014). CPU-only; these run on every operating system.

#include "engine/ecs/command_buffer.hpp"
#include "engine/ecs/registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstddef>
#include <memory>
#include <vector>

using engine::ecs::ApplyReport;
using engine::ecs::CommandBuffer;
using engine::ecs::EntityHandle;
using engine::ecs::PendingEntity;
using engine::ecs::RejectReason;
using engine::ecs::Registry;

namespace {

struct Position {
    Position() = default;
    Position(int a, int b) : x(a), y(b) {}
    int x = 0;
    int y = 0;
};

struct Velocity {
    Velocity() = default;
    Velocity(int a, int b) : dx(a), dy(b) {}
    int dx = 0;
    int dy = 0;
};

// Move-only component: the buffer must move it in, not copy it.
struct Owned {
    explicit Owned(int v) : value(std::make_unique<int>(v)) {}
    std::unique_ptr<int> value;
};

}  // namespace

TEST_CASE("A command buffer defers an add until apply", "[unit]") {
    Registry registry;
    const EntityHandle entity = registry.create();
    CommandBuffer buffer;

    buffer.add<Position>(entity, 1, 2);
    CHECK_FALSE(registry.has<Position>(entity));
    CHECK(buffer.size() == 1);

    const ApplyReport report = buffer.apply(registry);

    REQUIRE(registry.has<Position>(entity));
    CHECK(registry.get<Position>(entity)->x == 1);
    CHECK(registry.get<Position>(entity)->y == 2);
    CHECK(report.applied == 1);
    CHECK(report.rejected.empty());
    CHECK(buffer.empty());
}

TEST_CASE("Pending entities become real handles after apply", "[unit]") {
    Registry registry;
    CommandBuffer buffer;

    const PendingEntity first = buffer.create();
    const PendingEntity second = buffer.create();
    buffer.add<Position>(first, 3, 4);
    buffer.add<Velocity>(first, 5, 6);
    buffer.add<Position>(second, 7, 8);

    CHECK(registry.world().size() == 0);
    const ApplyReport report = buffer.apply(registry);

    REQUIRE(report.created.size() == 2);
    REQUIRE(registry.world().size() == 2);
    const EntityHandle a = report.created[first.id - 1];
    const EntityHandle b = report.created[second.id - 1];
    REQUIRE(registry.world().alive(a));
    REQUIRE(registry.world().alive(b));
    CHECK(registry.get<Position>(a)->x == 3);
    CHECK(registry.get<Velocity>(a)->dy == 6);
    CHECK_FALSE(registry.has<Velocity>(b));
    CHECK(registry.get<Position>(b)->y == 8);
}

TEST_CASE("A destroy command removes every component of the entity", "[unit]") {
    Registry registry;
    const EntityHandle entity = registry.create();
    registry.add<Position>(entity, 1, 1);
    registry.add<Velocity>(entity, 2, 2);
    CommandBuffer buffer;

    buffer.destroy(entity);
    CHECK(registry.world().alive(entity));
    const ApplyReport report = buffer.apply(registry);

    CHECK_FALSE(registry.world().alive(entity));
    CHECK(report.applied == 1);
    CHECK(registry.count<Position>() == 0);
    CHECK(registry.count<Velocity>() == 0);
}

TEST_CASE("Verification rejects a command on a stale handle and keeps the rest of the buffer", "[unit]") {
    Registry registry;
    const EntityHandle dead = registry.create();
    const EntityHandle live = registry.create();
    registry.destroy(dead);
    CommandBuffer buffer;

    buffer.add<Position>(dead, 1, 1);
    buffer.add<Position>(live, 2, 2);
    const ApplyReport report = buffer.apply(registry);

    REQUIRE(report.rejected.size() == 1);
    CHECK(report.rejected[0].command == 0);
    CHECK(report.rejected[0].reason == RejectReason::EntityNotAlive);
    CHECK(report.applied == 1);
    CHECK(registry.get<Position>(live)->x == 2);
}

TEST_CASE("A command buffer rejects a foreign-world handle that matches index and generation", "[unit]") {
    Registry owner;
    Registry other;
    const EntityHandle foreign = other.create();
    const EntityHandle local = owner.create();
    REQUIRE(foreign.index == local.index);
    REQUIRE(foreign.generation == local.generation);
    CommandBuffer buffer;

    buffer.add<Position>(foreign, 1, 1);
    buffer.destroy(foreign);
    const ApplyReport report = buffer.apply(owner);

    REQUIRE(report.rejected.size() == 2);
    CHECK(report.rejected[0].reason == RejectReason::EntityNotAlive);
    CHECK(report.rejected[1].reason == RejectReason::EntityNotAlive);
    CHECK(owner.world().alive(local));
    CHECK_FALSE(owner.has<Position>(local));
}

TEST_CASE("A command buffer rejects a stale handle whose slot was reused", "[unit]") {
    Registry registry;
    const EntityHandle old_handle = registry.create();
    registry.destroy(old_handle);
    const EntityHandle newer = registry.create();
    REQUIRE(newer.index == old_handle.index);
    CommandBuffer buffer;

    buffer.add<Position>(old_handle, 9, 9);
    const ApplyReport report = buffer.apply(registry);

    REQUIRE(report.rejected.size() == 1);
    CHECK(report.rejected[0].reason == RejectReason::EntityNotAlive);
    CHECK_FALSE(registry.has<Position>(newer));
}

TEST_CASE("A second destroy of the same entity in one buffer is rejected", "[unit]") {
    Registry registry;
    const EntityHandle entity = registry.create();
    CommandBuffer buffer;

    buffer.destroy(entity);
    buffer.destroy(entity);
    const ApplyReport report = buffer.apply(registry);

    CHECK(report.applied == 1);
    REQUIRE(report.rejected.size() == 1);
    CHECK(report.rejected[0].command == 1);
    CHECK(report.rejected[0].reason == RejectReason::EntityNotAlive);
    CHECK_FALSE(registry.world().alive(entity));
}

TEST_CASE("An add to an entity destroyed earlier in the same buffer is rejected", "[unit]") {
    Registry registry;
    const EntityHandle entity = registry.create();
    CommandBuffer buffer;

    buffer.destroy(entity);
    buffer.add<Position>(entity, 1, 1);
    const ApplyReport report = buffer.apply(registry);

    REQUIRE(report.rejected.size() == 1);
    CHECK(report.rejected[0].command == 1);
    CHECK(report.rejected[0].reason == RejectReason::EntityNotAlive);
    CHECK(registry.count<Position>() == 0);
}

TEST_CASE("Duplicate adds and missing removes are rejected by verification", "[unit]") {
    Registry registry;
    const EntityHandle entity = registry.create();
    registry.add<Position>(entity, 1, 1);
    CommandBuffer buffer;

    buffer.add<Position>(entity, 2, 2);        // already has one
    buffer.remove<Velocity>(entity);           // has none
    buffer.add<Velocity>(entity, 3, 3);        // accepted
    buffer.add<Velocity>(entity, 4, 4);        // duplicate of the add just accepted
    buffer.remove<Velocity>(entity);           // accepted: removes the component added above
    buffer.remove<Velocity>(entity);           // already removed by the command above
    const ApplyReport report = buffer.apply(registry);

    REQUIRE(report.rejected.size() == 4);
    CHECK(report.rejected[0].command == 0);
    CHECK(report.rejected[0].reason == RejectReason::ComponentPresent);
    CHECK(report.rejected[1].command == 1);
    CHECK(report.rejected[1].reason == RejectReason::ComponentAbsent);
    CHECK(report.rejected[2].command == 3);
    CHECK(report.rejected[2].reason == RejectReason::ComponentPresent);
    CHECK(report.rejected[3].command == 5);
    CHECK(report.rejected[3].reason == RejectReason::ComponentAbsent);
    CHECK(report.applied == 2);
    CHECK(registry.get<Position>(entity)->x == 1);
    CHECK_FALSE(registry.has<Velocity>(entity));
}

TEST_CASE("A pending entity destroyed in the same buffer is stale in the created list", "[unit]") {
    Registry registry;
    CommandBuffer buffer;

    const PendingEntity pending = buffer.create();
    buffer.add<Position>(pending, 1, 1);
    buffer.destroy(pending);
    buffer.add<Position>(pending, 2, 2);
    const ApplyReport report = buffer.apply(registry);

    REQUIRE(report.created.size() == 1);
    CHECK_FALSE(registry.world().alive(report.created[0]));
    REQUIRE(report.rejected.size() == 1);
    CHECK(report.rejected[0].command == 3);
    CHECK(report.rejected[0].reason == RejectReason::EntityNotAlive);
    CHECK(registry.world().size() == 0);
}

TEST_CASE("A command buffer accepts move-only components and is reusable after apply", "[unit]") {
    Registry registry;
    CommandBuffer buffer;

    const PendingEntity first = buffer.create();
    buffer.add<Owned>(first, 42);
    const ApplyReport report = buffer.apply(registry);

    REQUIRE(report.created.size() == 1);
    REQUIRE(registry.get<Owned>(report.created[0]) != nullptr);
    CHECK(*registry.get<Owned>(report.created[0])->value == 42);

    // apply() empties the buffer, so pending ids start again from 1.
    const PendingEntity again = buffer.create();
    CHECK(again.id == 1);
    CHECK(buffer.size() == 1);
}

TEST_CASE("Clearing a command buffer drops its commands without applying them", "[unit]") {
    Registry registry;
    const EntityHandle entity = registry.create();
    CommandBuffer buffer;

    buffer.destroy(entity);
    buffer.clear();

    CHECK(buffer.empty());
    const ApplyReport report = buffer.apply(registry);
    CHECK(report.applied == 0);
    CHECK(registry.world().alive(entity));
}
