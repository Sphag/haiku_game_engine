// Colliders, queries, and streamed collision cells (US-053) on the CPU: layer filters, triggers, raycasts and shape casts over every
// collider kind, collider replacement, queries across origin rebases, and cell loading that does not drop a body through unloaded
// geometry. These need no renderer and run on every operating system.
#include "engine/ecs/world.hpp"
#include "engine/physics/collision_cells.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/origin.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"
#include "engine/world/terrain.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using engine::ecs::EntityHandle;
using engine::ecs::World;
using engine::physics::BodyDesc;
using engine::physics::BodyMotion;
using engine::physics::CollisionCellDesc;
using engine::physics::CollisionCellStreamer;
using engine::physics::CollisionCells;
using engine::physics::kFixedStep;
using engine::world::CellData;
using engine::world::WorldCell;
using engine::physics::PhysicsError;
using engine::physics::PhysicsEvent;
using engine::physics::PhysicsEventKind;
using engine::physics::PhysicsWorld;
using engine::physics::PhysicsWorldDesc;
using engine::physics::QueryFilter;
using engine::physics::QueryHit;
using engine::physics::ShapeKind;
using engine::world::WorldOrigin;

namespace {

std::unique_ptr<PhysicsWorld> make_world(const PhysicsWorldDesc& desc = {}) {
    auto result = PhysicsWorld::create(desc);
    REQUIRE(result.ok());
    return std::move(result).value();
}

BodyDesc box(EntityHandle entity, BodyMotion motion, glm::dvec3 position, glm::vec3 half = glm::vec3(0.5f)) {
    BodyDesc desc;
    desc.entity = entity;
    desc.motion = motion;
    desc.shape = ShapeKind::Box;
    desc.half_extents = half;
    desc.position = position;
    return desc;
}

BodyDesc sphere(EntityHandle entity, BodyMotion motion, glm::dvec3 position, float radius) {
    BodyDesc desc;
    desc.entity = entity;
    desc.motion = motion;
    desc.shape = ShapeKind::Sphere;
    desc.radius = radius;
    desc.position = position;
    return desc;
}

EntityHandle add(PhysicsWorld& world, const BodyDesc& desc) {
    auto result = world.add_body(desc);
    REQUIRE(result.ok());
    return std::move(result).value();
}

void run_steps(PhysicsWorld& world, int steps) {
    for (int i = 0; i < steps; ++i) {
        REQUIRE(world.update(kFixedStep).ok());
    }
}

bool has_event(const std::vector<PhysicsEvent>& events, PhysicsEventKind kind, EntityHandle a, EntityHandle b) {
    return std::any_of(events.begin(), events.end(), [&](const PhysicsEvent& event) {
        return event.kind == kind && ((event.first == a && event.second == b) || (event.first == b && event.second == a));
    });
}

}  // namespace

TEST_CASE("A raycast returns the nearest body with its entity, world position, normal, and distance", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle near_box = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, 0.0, 0.0)));
    const EntityHandle far_box = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(10.0, 0.0, 0.0)));

    // The ray starts at x = -5, so the near box's -x face is at x = -0.5, 4.5 m away. The direction need not be normalised.
    const auto hit = world->raycast(glm::dvec3(-5.0, 0.0, 0.0), glm::vec3(3.0f, 0.0f, 0.0f), 50.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == near_box);
    CHECK(hit->position.x == Catch::Approx(-0.5).margin(1e-5));
    CHECK(hit->distance == Catch::Approx(4.5).margin(1e-4));
    CHECK(hit->normal.x == Catch::Approx(-1.0f).margin(1e-5f));
    CHECK_FALSE(hit->trigger);
    CHECK(hit->entity != far_box);

    // A max distance short of the near face finds nothing, and a ray that points away finds nothing.
    CHECK_FALSE(world->raycast(glm::dvec3(-5.0, 0.0, 0.0), glm::vec3(1.0f, 0.0f, 0.0f), 4.0f).has_value());
    CHECK_FALSE(world->raycast(glm::dvec3(-5.0, 0.0, 0.0), glm::vec3(-1.0f, 0.0f, 0.0f), 50.0f).has_value());
    CHECK_FALSE(world->raycast(glm::dvec3(-5.0, 0.0, 0.0), glm::vec3(0.0f), 50.0f).has_value());
}

TEST_CASE("Query filters select bodies by layer and ignore one entity", "[unit]") {
    World ecs;
    auto world = make_world();
    BodyDesc on_layer_two = box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, 0.0, 0.0));
    on_layer_two.layer = 2;
    const EntityHandle layered = add(*world, on_layer_two);
    const EntityHandle behind = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(5.0, 0.0, 0.0)));

    const glm::dvec3 origin(-5.0, 0.0, 0.0);
    const glm::vec3 dir(1.0f, 0.0f, 0.0f);

    // Only layer 0 is queried, so the layer-2 box is skipped and the box behind it is found.
    QueryFilter layer_zero;
    layer_zero.layers = 1u << 0;
    const auto skipped = world->raycast(origin, dir, 50.0f, layer_zero);
    REQUIRE(skipped.has_value());
    CHECK(skipped->entity == behind);

    QueryFilter layer_two;
    layer_two.layers = 1u << 2;
    const auto found = world->raycast(origin, dir, 50.0f, layer_two);
    REQUIRE(found.has_value());
    CHECK(found->entity == layered);

    // Ignoring the nearest body returns the one behind it.
    QueryFilter ignore_first;
    ignore_first.ignore = layered;
    const auto past = world->raycast(origin, dir, 50.0f, ignore_first);
    REQUIRE(past.has_value());
    CHECK(past->entity == behind);
}

TEST_CASE("Collision masks decide whether a dynamic body lands on or falls through a static body", "[unit]") {
    World ecs;
    auto world = make_world();
    // The floor sits on layer 3; its top is at y = 0.
    BodyDesc floor = box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(20.0f, 0.5f, 20.0f));
    floor.layer = 3;
    add(*world, floor);

    // The ball on layer 0 with every mask bit lands on the floor.
    const EntityHandle landing = ecs.create();
    add(*world, sphere(landing, BodyMotion::Dynamic, glm::dvec3(0.0, 3.0, 0.0), 0.5f));
    // The ball on layer 0 whose mask leaves out layer 3 passes through it.
    BodyDesc passing = sphere(ecs.create(), BodyMotion::Dynamic, glm::dvec3(4.0, 3.0, 0.0), 0.5f);
    passing.mask = ~(1u << 3);
    const EntityHandle falling = add(*world, passing);

    run_steps(*world, 180);

    const auto landed = world->step_transform(landing);
    REQUIRE(landed.has_value());
    CHECK(landed->position.y > 0.3);
    CHECK(landed->position.y < 0.7);

    const auto fell = world->step_transform(falling);
    REQUIRE(fell.has_value());
    CHECK(fell->position.y < -10.0);
}

TEST_CASE("A trigger reports contacts and is found by queries, but it does not push bodies", "[unit]") {
    World ecs;
    auto world = make_world();
    BodyDesc zone = box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, 0.0, 0.0), glm::vec3(5.0f, 0.5f, 5.0f));
    zone.trigger = true;
    const EntityHandle trigger = add(*world, zone);
    const EntityHandle ball = ecs.create();
    add(*world, sphere(ball, BodyMotion::Dynamic, glm::dvec3(0.0, 3.0, 0.0), 0.25f));

    std::vector<PhysicsEvent> events;
    for (int i = 0; i < 120; ++i) {
        REQUIRE(world->update(kFixedStep).ok());
        const auto batch = world->take_events();
        events.insert(events.end(), batch.begin(), batch.end());
    }

    CHECK(has_event(events, PhysicsEventKind::ContactAdded, trigger, ball));
    // The trigger does not stop the ball, which falls past it.
    const auto fallen = world->step_transform(ball);
    REQUIRE(fallen.has_value());
    CHECK(fallen->position.y < -0.5);

    // A ray sees the trigger and reports it; a query that excludes triggers finds nothing there.
    const auto seen = world->raycast(glm::dvec3(0.0, 10.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(seen.has_value());
    CHECK(seen->entity == trigger);
    CHECK(seen->trigger);

    QueryFilter solids_only;
    solids_only.include_triggers = false;
    CHECK_FALSE(world->raycast(glm::dvec3(0.0, 10.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f, solids_only).has_value());
}

TEST_CASE("Capsule, convex hull, mesh, and heightfield colliders are found by raycasts with their entities", "[unit]") {
    World ecs;
    auto world = make_world();

    BodyDesc capsule;
    capsule.entity = ecs.create();
    capsule.motion = BodyMotion::Static;
    capsule.shape = ShapeKind::Capsule;
    capsule.radius = 0.5f;
    capsule.half_height = 1.0f;
    capsule.position = glm::dvec3(0.0, 0.0, 0.0);
    const EntityHandle capsule_entity = add(*world, capsule);
    const auto on_capsule = world->raycast(glm::dvec3(0.0, 5.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(on_capsule.has_value());
    CHECK(on_capsule->entity == capsule_entity);
    // The top cap reaches half_height + radius = 1.5 m.
    CHECK(on_capsule->position.y == Catch::Approx(1.5).margin(1e-4));

    BodyDesc hull;
    hull.entity = ecs.create();
    hull.motion = BodyMotion::Static;
    hull.shape = ShapeKind::ConvexHull;
    hull.points = {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(-1.0f, -1.0f, -1.0f), glm::vec3(1.0f, -1.0f, -1.0f), glm::vec3(0.0f, -1.0f, 1.0f)};
    hull.position = glm::dvec3(10.0, 0.0, 0.0);
    const EntityHandle hull_entity = add(*world, hull);
    const auto on_hull = world->raycast(glm::dvec3(10.0, 5.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(on_hull.has_value());
    CHECK(on_hull->entity == hull_entity);
    CHECK(on_hull->position.y == Catch::Approx(1.0).margin(1e-4));

    // Two counter-clockwise (seen from above) triangles make a 10 m square at y = 0, centred on the origin.
    BodyDesc mesh;
    mesh.entity = ecs.create();
    mesh.motion = BodyMotion::Static;
    mesh.shape = ShapeKind::Mesh;
    mesh.vertices = {glm::vec3(-5.0f, 0.0f, -5.0f), glm::vec3(5.0f, 0.0f, -5.0f), glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(-5.0f, 0.0f, 5.0f)};
    mesh.indices = {0, 2, 1, 0, 3, 2};
    mesh.position = glm::dvec3(0.0, -20.0, 0.0);
    const EntityHandle mesh_entity = add(*world, mesh);
    const auto on_mesh = world->raycast(glm::dvec3(1.0, -15.0, 1.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(on_mesh.has_value());
    CHECK(on_mesh->entity == mesh_entity);
    CHECK(on_mesh->position.y == Catch::Approx(-20.0).margin(1e-4));
    CHECK(on_mesh->normal.y == Catch::Approx(1.0f).margin(1e-4f));

    // A 9 x 9 grid of samples at 1 m spacing, flat at 2 m, centred on x and z = 0.
    BodyDesc terrain;
    terrain.entity = ecs.create();
    terrain.motion = BodyMotion::Static;
    terrain.shape = ShapeKind::Heightfield;
    terrain.grid_size = 9;
    terrain.grid_spacing = 1.0f;
    terrain.heights.assign(9 * 9, 2.0f);
    terrain.position = glm::dvec3(-30.0, 0.0, 0.0);
    const EntityHandle terrain_entity = add(*world, terrain);
    const auto on_terrain = world->raycast(glm::dvec3(-30.0, 10.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
    REQUIRE(on_terrain.has_value());
    CHECK(on_terrain->entity == terrain_entity);
    CHECK(on_terrain->position.y == Catch::Approx(2.0).margin(1e-3));
    CHECK(on_terrain->normal.y == Catch::Approx(1.0f).margin(1e-4f));
}

TEST_CASE("Collider descriptions that cannot be built are rejected without changing the world", "[unit]") {
    World ecs;
    auto world = make_world();

    BodyDesc short_hull;
    short_hull.entity = ecs.create();
    short_hull.motion = BodyMotion::Static;
    short_hull.shape = ShapeKind::ConvexHull;
    short_hull.points = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    CHECK(world->add_body(short_hull).error() == PhysicsError::InvalidDesc);

    BodyDesc dynamic_mesh;
    dynamic_mesh.entity = ecs.create();
    dynamic_mesh.motion = BodyMotion::Dynamic;
    dynamic_mesh.shape = ShapeKind::Mesh;
    dynamic_mesh.vertices = {glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    dynamic_mesh.indices = {0, 1, 2};
    CHECK(world->add_body(dynamic_mesh).error() == PhysicsError::InvalidDesc);

    BodyDesc bad_heights;
    bad_heights.entity = ecs.create();
    bad_heights.motion = BodyMotion::Static;
    bad_heights.shape = ShapeKind::Heightfield;
    bad_heights.grid_size = 4;
    bad_heights.heights.assign(3, 0.0f);
    CHECK(world->add_body(bad_heights).error() == PhysicsError::InvalidDesc);

    BodyDesc bad_layer = sphere(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0), 0.5f);
    bad_layer.layer = 32;
    CHECK(world->add_body(bad_layer).error() == PhysicsError::InvalidDesc);

    CHECK(world->body_count() == 0);
    CHECK_FALSE(world->raycast(glm::dvec3(0.0, 5.0, 0.0), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f).has_value());
}

TEST_CASE("A shape cast finds the first body a moving shape reaches, with the entity and surface normal", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle floor = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(10.0f, 0.5f, 10.0f)));

    // A sphere of radius 0.5 centred 5 m above the floor touches it after 4.5 m.
    const BodyDesc probe = sphere(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 5.0, 0.0), 0.5f);
    const auto hit = world->shape_cast(probe, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == floor);
    CHECK(hit->distance == Catch::Approx(4.5).margin(1e-3));
    CHECK(hit->normal.y == Catch::Approx(1.0f).margin(1e-3f));

    CHECK_FALSE(world->shape_cast(probe, glm::vec3(0.0f, -1.0f, 0.0f), 3.0f).has_value());
    CHECK_FALSE(world->shape_cast(probe, glm::vec3(0.0f, 1.0f, 0.0f), 10.0f).has_value());
}

TEST_CASE("Replacing a collider keeps the entity and body but changes its shape; mismatched replacements are refused", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle target = ecs.create();
    add(*world, box(target, BodyMotion::Static, glm::dvec3(0.0, 0.0, 0.0)));

    const glm::dvec3 origin(-5.0, 0.0, 0.0);
    const glm::vec3 dir(1.0f, 0.0f, 0.0f);
    const auto before = world->raycast(origin, dir, 50.0f);
    REQUIRE(before.has_value());
    CHECK(before->distance == Catch::Approx(4.5).margin(1e-4));

    // A small sphere at the same centre is found further along the ray, at x = -0.1.
    const auto replaced = world->replace_collider(target, sphere(target, BodyMotion::Static, glm::dvec3(0.0), 0.1f));
    REQUIRE(replaced.ok());
    CHECK(replaced.value() == target);
    CHECK(world->has_body(target));
    const auto after = world->raycast(origin, dir, 50.0f);
    REQUIRE(after.has_value());
    CHECK(after->entity == target);
    CHECK(after->distance == Catch::Approx(4.9).margin(1e-4));

    CHECK(world->replace_collider(ecs.create(), sphere(target, BodyMotion::Static, glm::dvec3(0.0), 0.1f)).error() ==
          PhysicsError::InvalidEntity);
    CHECK(world->replace_collider(target, box(target, BodyMotion::Dynamic, glm::dvec3(0.0))).error() == PhysicsError::InvalidDesc);
    CHECK(world->replace_collider(target, sphere(ecs.create(), BodyMotion::Static, glm::dvec3(0.0), 0.1f)).error() ==
          PhysicsError::InvalidEntity);

    // The refused replacement left the small sphere in place.
    const auto still = world->raycast(origin, dir, 50.0f);
    REQUIRE(still.has_value());
    CHECK(still->distance == Catch::Approx(4.9).margin(1e-4));
}

TEST_CASE("Queries return the same entities and world positions before and after an origin rebase", "[unit]") {
    World ecs;
    WorldOrigin origin;
    auto world = make_world();
    origin.subscribe(world->origin_subscriber());

    // Far from the origin, a single-precision position has about a 0.5 mm step.
    const glm::dvec3 base(5000.25, 0.0, -3000.5);
    const EntityHandle target = ecs.create();
    add(*world, box(target, BodyMotion::Static, base + glm::dvec3(0.0, 0.0, 0.0)));
    const EntityHandle floor = ecs.create();
    add(*world, box(floor, BodyMotion::Static, base + glm::dvec3(0.0, -2.0, 0.0), glm::vec3(10.0f, 0.5f, 10.0f)));

    const glm::dvec3 ray_origin = base + glm::dvec3(-5.0, 0.0, 0.0);
    const auto before = world->raycast(ray_origin, glm::vec3(1.0f, 0.0f, 0.0f), 50.0f);
    REQUIRE(before.has_value());
    CHECK(before->entity == target);

    // Beside the target box, so the cast meets the floor rather than the box.
    const BodyDesc probe = sphere(ecs.create(), BodyMotion::Dynamic, base + glm::dvec3(5.0, 5.0, 0.0), 0.5f);
    const auto cast_before = world->shape_cast(probe, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    REQUIRE(cast_before.has_value());
    CHECK(cast_before->entity == floor);

    // Move the origin onto the target: the camera is beyond the rebase distance.
    REQUIRE(origin.update(base).has_value());
    CHECK(world->origin() == base);

    const auto after = world->raycast(ray_origin, glm::vec3(1.0f, 0.0f, 0.0f), 50.0f);
    REQUIRE(after.has_value());
    CHECK(after->entity == before->entity);
    CHECK(after->position.x == Catch::Approx(before->position.x).margin(1e-3));
    CHECK(after->position.z == Catch::Approx(before->position.z).margin(1e-3));
    CHECK(after->distance == Catch::Approx(before->distance).margin(1e-4));

    const auto cast_after = world->shape_cast(probe, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
    REQUIRE(cast_after.has_value());
    CHECK(cast_after->entity == cast_before->entity);
    CHECK(cast_after->position.y == Catch::Approx(cast_before->position.y).margin(1e-3));
}

TEST_CASE("A cell loads its colliders, and an unload waits while a moving body is in its column", "[unit]") {
    World ecs;
    auto world = make_world();
    CollisionCells cells(*world, 2.0);

    const EntityHandle floor = ecs.create();
    CollisionCellDesc cell;
    cell.id = 7;
    cell.min = glm::dvec3(-10.0, -1.0, -10.0);
    cell.max = glm::dvec3(10.0, 0.0, 10.0);
    cell.bodies = {box(floor, BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(10.0f, 0.5f, 10.0f))};

    auto loaded = cells.load(cell);
    REQUIRE(loaded.ok());
    CHECK(loaded.value() == 1);
    CHECK(world->has_body(floor));
    CHECK(cells.load(cell).error() == PhysicsError::InvalidDesc);

    // A ball 20 m above the cell is falling toward it, so the unload waits for the ball.
    const EntityHandle ball = ecs.create();
    add(*world, sphere(ball, BodyMotion::Dynamic, glm::dvec3(0.0, 20.0, 0.0), 0.5f));
    CHECK(cells.unload(7));
    CHECK(cells.unload_pending(7));
    CHECK(world->has_body(floor));

    // The ball lands on the cell and rests there: it does not fall through the unloaded geometry.
    run_steps(*world, 400);
    cells.update();
    CHECK(cells.unload_pending(7));
    const auto resting = world->step_transform(ball);
    REQUIRE(resting.has_value());
    CHECK(resting->position.y > 0.3);
    CHECK(world->has_body(floor));

    // Once the ball is gone, the column is clear and the next update completes the unload.
    CHECK(world->remove_body(ball));
    cells.update();
    CHECK_FALSE(cells.loaded(7));
    CHECK_FALSE(world->has_body(floor));
    CHECK(cells.cell_count() == 0);
    CHECK_FALSE(cells.unload(7));
}

TEST_CASE("An unload with no moving body nearby removes the cell at once; a failed load leaves no bodies", "[unit]") {
    World ecs;
    auto world = make_world();
    CollisionCells cells(*world, 2.0);

    const EntityHandle a = ecs.create();
    const EntityHandle b = ecs.create();
    CollisionCellDesc cell;
    cell.id = 1;
    cell.min = glm::dvec3(0.0);
    cell.max = glm::dvec3(10.0);
    cell.bodies = {box(a, BodyMotion::Static, glm::dvec3(2.0)), box(b, BodyMotion::Static, glm::dvec3(4.0))};
    REQUIRE(cells.load(cell).ok());
    CHECK(world->body_count() == 2);
    CHECK(cells.unload(1));
    CHECK_FALSE(cells.loaded(1));
    CHECK(world->body_count() == 0);

    // A duplicate entity fails the load; the first body is rolled back so the world is unchanged.
    CollisionCellDesc clash;
    clash.id = 2;
    clash.min = glm::dvec3(0.0);
    clash.max = glm::dvec3(10.0);
    const EntityHandle c = ecs.create();
    clash.bodies = {box(c, BodyMotion::Static, glm::dvec3(2.0)), box(c, BodyMotion::Static, glm::dvec3(4.0))};
    CHECK(cells.load(clash).error() == PhysicsError::InvalidEntity);
    CHECK_FALSE(cells.loaded(2));
    CHECK(world->body_count() == 0);
}

TEST_CASE("A cell's unload completes once a body that was in its column leaves it", "[unit]") {
    World ecs;
    auto world = make_world();
    CollisionCells cells(*world, 1.0);

    CollisionCellDesc cell;
    cell.id = 3;
    cell.min = glm::dvec3(0.0);
    cell.max = glm::dvec3(4.0);
    cell.bodies = {box(ecs.create(), BodyMotion::Static, glm::dvec3(2.0, -0.5, 2.0), glm::vec3(2.0f, 0.5f, 2.0f))};
    REQUIRE(cells.load(cell).ok());

    // A kinematic platform stands inside the column, so the unload waits.
    const EntityHandle platform = ecs.create();
    add(*world, box(platform, BodyMotion::Kinematic, glm::dvec3(2.0, 0.0, 2.0), glm::vec3(0.5f)));
    CHECK(cells.unload(3));
    cells.update();
    CHECK(cells.unload_pending(3));

    // Moving it far outside the column (margin 1 m) lets the unload complete.
    REQUIRE(world->move_kinematic(platform, glm::dvec3(50.0, 0.0, 50.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
    run_steps(*world, 60);
    cells.update();
    CHECK_FALSE(cells.loaded(3));
}

TEST_CASE("A terrain tile's heightfield cell is hit by a ray at the terrain height, and a tile outside the terrain is refused", "[unit]") {
    World ecs;
    auto world = make_world();
    CollisionCells cells(*world);

    engine::world::TerrainDesc terrain_desc;
    terrain_desc.tiles_x = 2;
    terrain_desc.tiles_z = 2;
    terrain_desc.tile_cells = 8;
    terrain_desc.cell_size = 1.0f;
    auto created = engine::world::create_terrain(terrain_desc);
    REQUIRE(created.ok());
    engine::world::Terrain terrain = std::move(created).value();
    // A plane, so the heightfield's triangulation does not change the expected height.
    for (std::uint32_t iz = 0; iz < terrain.samples_z; ++iz) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ++ix) {
            terrain.heights[engine::world::sample_index(terrain, ix, iz)] = 1.0f + 0.25f * static_cast<float>(ix) + 0.5f * static_cast<float>(iz);
        }
    }

    const EntityHandle tile = ecs.create();
    auto built = engine::physics::terrain_tile_cell(terrain, {1, 0}, 11, tile);
    REQUIRE(built.ok());
    CollisionCellDesc cell = std::move(built).value();
    CHECK(cell.min.x == Catch::Approx(8.0));
    CHECK(cell.max.x == Catch::Approx(16.0));
    CHECK(cell.min.z == Catch::Approx(0.0));
    CHECK(cell.max.z == Catch::Approx(8.0));
    REQUIRE(cells.load(cell).ok());

    // Sample (10.5, 3.5) sits inside the tile at x = 8..16, z = 0..8: its height is 1 + 0.25 * 10.5 + 0.5 * 3.5.
    const auto hit = world->raycast(glm::dvec3(10.5, 100.0, 3.5), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == tile);
    CHECK_THAT(hit->position.y, WithinAbs(1.0 + 0.25 * 10.5 + 0.5 * 3.5, 0.05));
    CHECK_THAT(hit->position.x, WithinAbs(10.5, 1.0e-3));
    CHECK_THAT(hit->position.z, WithinAbs(3.5, 1.0e-3));

    // A tile past the last column is refused.
    auto outside = engine::physics::terrain_tile_cell(terrain, {2, 0}, 12, ecs.create());
    CHECK_FALSE(outside.ok());
    CHECK(outside.error() == PhysicsError::InvalidDesc);
}

TEST_CASE("A streamed cell loads on activation, keeps its bodies through a reactivation, and unloads at once when clear", "[unit]") {
    World ecs;
    auto world = make_world();
    CollisionCells cells(*world, 1.0);
    engine::world::WorldDesc world_desc;
    world_desc.cell_size = 10.0f;

    CollisionCellStreamer streamer(cells, world_desc, [&ecs](const WorldCell& cell) {
        std::vector<BodyDesc> bodies;
        const glm::dvec3 centre(cell.coord.x * 10.0 + 5.0, -0.5, cell.coord.z * 10.0 + 5.0);
        bodies.push_back(box(ecs.create(), BodyMotion::Static, centre, glm::vec3(5.0f, 0.5f, 5.0f)));
        return bodies;
    });

    auto make_cell = [](std::int32_t x, std::int32_t z) {
        auto cell = std::make_shared<WorldCell>();
        cell->coord = {x, z};
        return CellData(cell);
    };
    const CellData near = make_cell(0, 0);
    const CellData far = make_cell(2, 0);
    const std::uint64_t near_id = CollisionCellStreamer::cell_key({0, 0});
    const std::uint64_t far_id = CollisionCellStreamer::cell_key({2, 0});

    streamer.on_activated(near);
    CHECK(cells.loaded(near_id));
    CHECK(world->body_count() == 1);

    // A ball above the near cell keeps it resident: a deactivation waits, and a reactivation cancels the wait.
    const EntityHandle ball = add(*world, sphere(ecs.create(), BodyMotion::Dynamic, glm::dvec3(5.0, 3.0, 5.0), 0.5f));
    streamer.on_deactivated(near);
    CHECK(cells.unload_pending(near_id));
    CHECK(world->body_count() == 2);
    streamer.on_activated(near);
    CHECK_FALSE(cells.unload_pending(near_id));
    CHECK(cells.loaded(near_id));
    CHECK(world->body_count() == 2);
    CHECK(world->has_body(ball));

    // A cell with no moving body nearby unloads at the deactivation.
    streamer.on_activated(far);
    CHECK(cells.loaded(far_id));
    streamer.on_deactivated(far);
    CHECK_FALSE(cells.loaded(far_id));
    CHECK(world->body_count() == 2);
    CHECK(streamer.failed_loads() == 0);
}

TEST_CASE("A streamed cell whose bodies the world refuses stays unloaded and counts the failure", "[unit]") {
    World ecs;
    auto world = make_world();
    CollisionCells cells(*world);
    CollisionCellStreamer streamer(cells, engine::world::WorldDesc{}, [](const WorldCell&) {
        // An invalid entity is refused by the world, so the whole cell fails to load.
        return std::vector<BodyDesc>{box(EntityHandle{}, BodyMotion::Static, glm::dvec3(0.0))};
    });

    const CellData cell = [] {
        auto made = std::make_shared<WorldCell>();
        made->coord = {4, 4};
        return CellData(made);
    }();
    streamer.on_activated(cell);
    CHECK_FALSE(cells.loaded(CollisionCellStreamer::cell_key({4, 4})));
    CHECK(streamer.failed_loads() == 1);
    CHECK(world->body_count() == 0);
}
