// Component pools, registry, and queries (US-013). CPU-only; these run on every operating system.

#include "engine/ecs/entity.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/ecs/sparse_set.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

using engine::ecs::EntityHandle;
using engine::ecs::Registry;
using engine::ecs::SparseSet;

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

struct Health {
    Health() = default;
    explicit Health(int v) : hp(v) {}
    int hp = 0;
};

// Over-aligned component: alignment must survive growth, which moves every element.
struct alignas(64) Matrix {
    float values[16] = {};
};

// Move-only component that owns a resource.
struct Owned {
    explicit Owned(int v) : value(std::make_unique<int>(v)) {}
    std::unique_ptr<int> value;
};

// Counts live instances so tests can check that every constructed object is destroyed exactly once.
struct Tracked {
    static inline int live = 0;
    explicit Tracked(int v) : value(v) { ++live; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
    Tracked& operator=(Tracked&& other) noexcept {
        value = other.value;
        return *this;
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    ~Tracked() { --live; }
    int value;
};

static_assert(!std::is_copy_constructible_v<SparseSet<Owned>>, "pools must never duplicate components");
static_assert(std::is_nothrow_move_constructible_v<SparseSet<Position>>);

// Builds a registry with a known population of entities, so tests can assert exact query membership.
struct Scene {
    Registry registry;
    EntityHandle with_all;     // Position, Velocity, Health
    EntityHandle moving;       // Position, Velocity
    EntityHandle still;        // Position only
    EntityHandle damage_only;  // Health only
    EntityHandle empty;        // no components

    Scene() {
        with_all = registry.create();
        moving = registry.create();
        still = registry.create();
        damage_only = registry.create();
        empty = registry.create();

        registry.add<Position>(with_all, 1, 2);
        registry.add<Velocity>(with_all, 3, 4);
        registry.add<Health>(with_all, 100);
        registry.add<Position>(moving, 5, 6);
        registry.add<Velocity>(moving, 7, 8);
        registry.add<Position>(still, 9, 10);
        registry.add<Health>(damage_only, 50);
    }
};

}  // namespace

TEST_CASE("A sparse set adds, gets, and removes one member", "[unit]") {
    SparseSet<Position> set;
    const EntityHandle a{0, 1, 1};

    CHECK(set.empty());
    Position* added = set.emplace(a, 3, 4);
    REQUIRE(added != nullptr);
    CHECK(added->x == 3);
    CHECK(set.contains(a));
    CHECK(set.size() == 1);

    CHECK(set.get(a) != nullptr);
    CHECK(set.get(a)->y == 4);

    CHECK(set.remove(a));
    CHECK_FALSE(set.contains(a));
    CHECK(set.get(a) == nullptr);
    CHECK(set.size() == 0);
    CHECK_FALSE(set.remove(a));
}

TEST_CASE("A sparse set refuses a second component for the same entity and keeps the first", "[unit]") {
    SparseSet<Position> set;
    const EntityHandle a{0, 1, 1};

    REQUIRE(set.emplace(a, 1, 1) != nullptr);
    CHECK(set.emplace(a, 9, 9) == nullptr);

    CHECK(set.size() == 1);
    CHECK(set.get(a)->x == 1);
}

TEST_CASE("A sparse set rejects a stale handle that shares a slot with a newer entity", "[unit]") {
    SparseSet<Position> set;
    const EntityHandle old_entity{4, 1, 1};
    const EntityHandle new_entity{4, 2, 1};

    REQUIRE(set.emplace(old_entity, 7, 7) != nullptr);

    CHECK_FALSE(set.contains(new_entity));
    CHECK(set.get(new_entity) == nullptr);
    CHECK_FALSE(set.remove(new_entity));
    CHECK(set.contains(old_entity));
}

TEST_CASE("Adding a component over a stale entry replaces it so each slot maps to one member", "[unit]") {
    SparseSet<Position> set;
    const EntityHandle old_entity{4, 1, 1};
    const EntityHandle new_entity{4, 2, 1};

    REQUIRE(set.emplace(old_entity, 7, 7) != nullptr);
    REQUIRE(set.emplace(new_entity, 8, 8) != nullptr);

    CHECK(set.size() == 1);
    CHECK_FALSE(set.contains(old_entity));
    CHECK(set.get(new_entity)->x == 8);
}

TEST_CASE("Removing members from the middle keeps every other member reachable", "[unit]") {
    constexpr std::uint32_t kCount = 300;
    SparseSet<Position> set;
    std::vector<EntityHandle> entities;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        const EntityHandle entity{i, 1, 1};
        entities.push_back(entity);
        REQUIRE(set.emplace(entity, static_cast<int>(i), 0) != nullptr);
    }

    // Remove every third member, which exercises swap-with-last at many positions, including the last one.
    for (std::uint32_t i = 0; i < kCount; i += 3) {
        REQUIRE(set.remove(entities[i]));
    }

    CHECK(set.size() == kCount - (kCount + 2) / 3);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        if (i % 3 == 0) {
            CHECK_FALSE(set.contains(entities[i]));
        } else {
            REQUIRE(set.get(entities[i]) != nullptr);
            CHECK(set.get(entities[i])->x == static_cast<int>(i));
        }
    }
}

TEST_CASE("Components keep their alignment across growth and removal", "[unit]") {
    constexpr std::uint32_t kCount = 1000;
    SparseSet<Matrix> set;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        Matrix* matrix = set.emplace(EntityHandle{i, 1, 1});
        REQUIRE(matrix != nullptr);
        matrix->values[0] = static_cast<float>(i);
    }

    for (std::uint32_t i = 0; i < kCount; ++i) {
        const Matrix* matrix = set.get(EntityHandle{i, 1, 1});
        REQUIRE(matrix != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(matrix) % alignof(Matrix) == 0);
        CHECK(matrix->values[0] == static_cast<float>(i));
    }

    REQUIRE(set.remove(EntityHandle{0, 1, 1}));
    const Matrix* moved = set.get(EntityHandle{kCount - 1, 1, 1});
    REQUIRE(moved != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(moved) % alignof(Matrix) == 0);
}

TEST_CASE("Move-only components survive growth, moves on removal, and clear", "[unit]") {
    constexpr std::uint32_t kCount = 500;
    SparseSet<Owned> set;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        REQUIRE(set.emplace(EntityHandle{i, 1, 1}, static_cast<int>(i)) != nullptr);
    }

    // Removing the first entity moves the last component into its place; the moved value must stay intact.
    REQUIRE(set.remove(EntityHandle{0, 1, 1}));
    for (std::uint32_t i = 1; i < kCount; ++i) {
        const Owned* item = set.get(EntityHandle{i, 1, 1});
        REQUIRE(item != nullptr);
        REQUIRE(item->value != nullptr);
        CHECK(*item->value == static_cast<int>(i));
    }

    set.clear();
    CHECK(set.empty());
    CHECK(set.get(EntityHandle{1, 1, 1}) == nullptr);
    REQUIRE(set.emplace(EntityHandle{1, 1, 1}, 42) != nullptr);
    CHECK(*set.get(EntityHandle{1, 1, 1})->value == 42);
}

TEST_CASE("Removing and clearing run component destructors exactly once", "[unit]") {
    const int baseline = Tracked::live;
    {
        SparseSet<Tracked> set;
        for (std::uint32_t i = 0; i < 64; ++i) {
            REQUIRE(set.emplace(EntityHandle{i, 1, 1}, static_cast<int>(i)) != nullptr);
        }
        CHECK(Tracked::live == baseline + 64);

        REQUIRE(set.remove(EntityHandle{10, 1, 1}));
        CHECK(Tracked::live == baseline + 63);

        set.clear();
        CHECK(Tracked::live == baseline);

        REQUIRE(set.emplace(EntityHandle{0, 1, 1}, 1) != nullptr);
        CHECK(Tracked::live == baseline + 1);
    }
    CHECK(Tracked::live == baseline);
}

TEST_CASE("Destroying a registry destroys every component it still holds", "[unit]") {
    const int baseline = Tracked::live;
    {
        Registry registry;
        for (int i = 0; i < 10; ++i) {
            const EntityHandle entity = registry.create();
            REQUIRE(registry.add<Tracked>(entity, i) != nullptr);
        }
        CHECK(Tracked::live == baseline + 10);
    }
    CHECK(Tracked::live == baseline);
}

TEST_CASE("Destroying an entity removes its components from every pool", "[unit]") {
    Scene scene;
    REQUIRE(scene.registry.has<Position>(scene.with_all));
    REQUIRE(scene.registry.has<Health>(scene.with_all));

    CHECK(scene.registry.destroy(scene.with_all));

    CHECK_FALSE(scene.registry.has<Position>(scene.with_all));
    CHECK_FALSE(scene.registry.has<Velocity>(scene.with_all));
    CHECK_FALSE(scene.registry.has<Health>(scene.with_all));
    CHECK(scene.registry.count<Position>() == 2);
    CHECK(scene.registry.count<Velocity>() == 1);
    CHECK(scene.registry.count<Health>() == 1);
    CHECK_FALSE(scene.registry.destroy(scene.with_all));
}

TEST_CASE("A reused slot does not inherit components of the destroyed entity", "[unit]") {
    Registry registry;
    const EntityHandle first = registry.create();
    REQUIRE(registry.add<Health>(first, 77) != nullptr);
    REQUIRE(registry.destroy(first));

    const EntityHandle second = registry.create();

    CHECK(second.index == first.index);
    CHECK_FALSE(registry.has<Health>(second));
    CHECK(registry.get<Health>(first) == nullptr);
    CHECK(registry.count<Health>() == 0);
}

TEST_CASE("Component access through a stale or foreign handle is rejected", "[unit]") {
    Scene scene;
    Registry other;
    const EntityHandle foreign = other.create();

    CHECK(scene.registry.add<Health>(foreign, 1) == nullptr);
    CHECK(scene.registry.get<Position>(foreign) == nullptr);
    CHECK_FALSE(scene.registry.remove<Position>(foreign));
    CHECK(scene.registry.count<Health>() == 2);

    REQUIRE(scene.registry.destroy(scene.still));
    CHECK(scene.registry.add<Position>(scene.still, 0, 0) == nullptr);
    CHECK(scene.registry.get<Position>(scene.still) == nullptr);
}

TEST_CASE("Query returns the intersection of every listed component", "[unit]") {
    Scene scene;

    const auto pos_vel = scene.registry.query<Position, Velocity>();
    const std::vector<EntityHandle> pos_vel_expected{scene.with_all, scene.moving};
    CHECK_THAT(pos_vel, Catch::Matchers::UnorderedEquals(pos_vel_expected));

    const auto pos_vel_hp = scene.registry.query<Position, Velocity, Health>();
    const std::vector<EntityHandle> pos_vel_hp_expected{scene.with_all};
    CHECK_THAT(pos_vel_hp, Catch::Matchers::UnorderedEquals(pos_vel_hp_expected));

    const auto pos = scene.registry.query<Position>();
    const std::vector<EntityHandle> pos_expected{scene.with_all, scene.moving, scene.still};
    CHECK_THAT(pos, Catch::Matchers::UnorderedEquals(pos_expected));

    const auto hp_vel = scene.registry.query<Health, Velocity>();
    const std::vector<EntityHandle> hp_vel_expected{scene.with_all};
    CHECK_THAT(hp_vel, Catch::Matchers::UnorderedEquals(hp_vel_expected));
}

TEST_CASE("Query membership is independent of the order components were added", "[unit]") {
    Registry forward;
    Registry reverse;
    const EntityHandle a = forward.create();
    const EntityHandle b = forward.create();
    const EntityHandle c = reverse.create();
    const EntityHandle d = reverse.create();

    REQUIRE(forward.add<Position>(a, 0, 0) != nullptr);
    REQUIRE(forward.add<Velocity>(a, 0, 0) != nullptr);
    REQUIRE(forward.add<Velocity>(b, 0, 0) != nullptr);
    REQUIRE(forward.add<Position>(b, 0, 0) != nullptr);

    REQUIRE(reverse.add<Velocity>(c, 0, 0) != nullptr);
    REQUIRE(reverse.add<Position>(c, 0, 0) != nullptr);
    REQUIRE(reverse.add<Position>(d, 0, 0) != nullptr);
    REQUIRE(reverse.add<Velocity>(d, 0, 0) != nullptr);

    const auto forward_hits = forward.query<Position, Velocity>();
    const std::vector<EntityHandle> forward_expected{a, b};
    CHECK_THAT(forward_hits, Catch::Matchers::UnorderedEquals(forward_expected));

    const auto reverse_hits = reverse.query<Velocity, Position>();
    const std::vector<EntityHandle> reverse_expected{c, d};
    CHECK_THAT(reverse_hits, Catch::Matchers::UnorderedEquals(reverse_expected));
}

TEST_CASE("Removing a component drops the entity from queries that need it", "[unit]") {
    Scene scene;

    REQUIRE(scene.registry.remove<Velocity>(scene.moving));

    const auto pos_vel = scene.registry.query<Position, Velocity>();
    const std::vector<EntityHandle> pos_vel_expected{scene.with_all};
    CHECK_THAT(pos_vel, Catch::Matchers::UnorderedEquals(pos_vel_expected));

    const auto pos = scene.registry.query<Position>();
    const std::vector<EntityHandle> pos_expected{scene.with_all, scene.moving, scene.still};
    CHECK_THAT(pos, Catch::Matchers::UnorderedEquals(pos_expected));

    CHECK_FALSE(scene.registry.remove<Velocity>(scene.moving));
}

TEST_CASE("Destroyed and foreign entities never appear in query results", "[unit]") {
    Scene scene;
    Registry other;
    const EntityHandle foreign = other.create();
    REQUIRE(other.add<Position>(foreign, 0, 0) != nullptr);

    REQUIRE(scene.registry.destroy(scene.moving));

    const auto hits = scene.registry.query<Position>();
    const std::vector<EntityHandle> expected{scene.with_all, scene.still};
    CHECK_THAT(hits, Catch::Matchers::UnorderedEquals(expected));
    for (const EntityHandle entity : hits) {
        CHECK(scene.registry.world().alive(entity));
        CHECK(entity != foreign);
    }
}

TEST_CASE("Empty queries return no entities without touching storage", "[unit]") {
    Scene scene;

    // No type list at all.
    CHECK(scene.registry.query<>().empty());

    // A component type that no entity has ever held.
    CHECK(scene.registry.query<Position, Owned>().empty());

    // A component type whose pool exists but has no members left.
    REQUIRE(scene.registry.remove<Health>(scene.with_all));
    REQUIRE(scene.registry.remove<Health>(scene.damage_only));
    CHECK(scene.registry.count<Health>() == 0);
    CHECK(scene.registry.query<Health>().empty());
    CHECK(scene.registry.query<Health, Position>().empty());
    CHECK(scene.registry.count<Owned>() == 0);

    // A registry with no entities at all.
    Registry empty_registry;
    CHECK(empty_registry.query<Position>().empty());
    CHECK(empty_registry.query<Position, Velocity>().empty());
}

TEST_CASE("Entities without components never match a query", "[unit]") {
    Scene scene;

    CHECK_FALSE(scene.registry.has<Position>(scene.empty));
    const auto hits = scene.registry.query<Position>();
    CHECK_THAT(hits, !Catch::Matchers::VectorContains(scene.empty));
}

TEST_CASE("Bulk registry population and churn keep query membership exact", "[unit]") {
    constexpr int kCount = 4000;
    Registry registry;
    std::vector<EntityHandle> entities;
    entities.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        const EntityHandle entity = registry.create();
        entities.push_back(entity);
        REQUIRE(registry.add<Position>(entity, i, 0) != nullptr);
        if (i % 2 == 0) {
            REQUIRE(registry.add<Velocity>(entity, 0, i) != nullptr);
        }
    }
    CHECK(registry.count<Position>() == static_cast<std::size_t>(kCount));
    CHECK(registry.query<Position, Velocity>().size() == static_cast<std::size_t>(kCount / 2));

    // Destroy every fourth entity, which are all in the query, then check that the query reflects only what remains.
    for (int i = 0; i < kCount; i += 4) {
        REQUIRE(registry.destroy(entities[i]));
    }
    std::vector<EntityHandle> remaining;
    for (int i = 0; i < kCount; ++i) {
        if (i % 4 == 2) {
            remaining.push_back(entities[i]);
        }
    }

    const auto hits = registry.query<Position, Velocity>();
    CHECK_THAT(hits, Catch::Matchers::UnorderedEquals(remaining));
    CHECK(hits.size() == remaining.size());
    CHECK(registry.count<Position>() == static_cast<std::size_t>(kCount - kCount / 4));
}

TEST_CASE("Registry component mutation through get writes the stored value", "[unit]") {
    Scene scene;

    Health* health = scene.registry.get<Health>(scene.damage_only);
    REQUIRE(health != nullptr);
    health->hp -= 25;

    const Registry& const_view = scene.registry;
    REQUIRE(const_view.get<Health>(scene.damage_only) != nullptr);
    CHECK(const_view.get<Health>(scene.damage_only)->hp == 25);
}

TEST_CASE("A moved registry keeps its entities, components, and world identity", "[unit]") {
    Scene scene;
    const std::uint32_t original_world = scene.registry.world().id();

    Registry target = std::move(scene.registry);

    CHECK(target.world().id() == original_world);
    CHECK(target.world().alive(scene.with_all));
    CHECK(target.has<Health>(scene.with_all));
    const auto hits = target.query<Position, Velocity>();
    const std::vector<EntityHandle> expected{scene.with_all, scene.moving};
    CHECK_THAT(hits, Catch::Matchers::UnorderedEquals(expected));
}
