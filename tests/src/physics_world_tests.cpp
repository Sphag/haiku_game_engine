// Physics adapter (US-052) on the CPU: a Jolt world per scene, the fixed 60 Hz step, pause, the catch-up limit, interpolated render
// transforms, stable entity handles, the callback queue, origin rebasing, and world closure. These need no renderer and run on every
// operating system.
#include "engine/ecs/world.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/origin.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using engine::ecs::EntityHandle;
using engine::ecs::World;
using engine::physics::BodyDesc;
using engine::physics::BodyMotion;
using engine::physics::kDefaultMaxCatchUpSteps;
using engine::physics::kFixedRateHz;
using engine::physics::kFixedStep;
using engine::physics::PhysicsError;
using engine::physics::PhysicsEvent;
using engine::physics::PhysicsEventKind;
using engine::physics::PhysicsTransform;
using engine::physics::PhysicsWorld;
using engine::physics::PhysicsWorldDesc;
using engine::physics::ShapeKind;
using engine::world::OriginShift;
using engine::world::WorldOrigin;

namespace {

std::unique_ptr<PhysicsWorld> make_world(const PhysicsWorldDesc& desc = {}) {
    auto result = PhysicsWorld::create(desc);
    REQUIRE(result.ok());
    return std::move(result).value();
}

BodyDesc box_at(EntityHandle entity, BodyMotion motion, glm::dvec3 position, glm::vec3 half = glm::vec3(0.5f)) {
    BodyDesc desc;
    desc.entity = entity;
    desc.motion = motion;
    desc.shape = ShapeKind::Box;
    desc.half_extents = half;
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

bool contains_pair(const std::vector<PhysicsEvent>& events, PhysicsEventKind kind, EntityHandle a, EntityHandle b) {
    return std::any_of(events.begin(), events.end(), [&](const PhysicsEvent& event) {
        return event.kind == kind && ((event.first == a && event.second == b) || (event.first == b && event.second == a));
    });
}

}  // namespace

TEST_CASE("The fixed step is 60 Hz and an update runs the whole steps its frame time covers", "[unit]") {
    CHECK(kFixedRateHz == 60.0);
    CHECK_THAT(kFixedStep, WithinAbs(1.0 / 60.0, 1e-12));

    World ecs;
    auto world = make_world();
    add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));

    REQUIRE(world->update(3.5 * kFixedStep).ok());
    CHECK(world->stats().steps == 3);
    CHECK_THAT(world->interpolation_alpha(), WithinAbs(0.5, 1e-6));

    REQUIRE(world->update(0.5 * kFixedStep).ok());
    CHECK(world->stats().steps == 4);
    CHECK_THAT(world->interpolation_alpha(), WithinAbs(0.0, 1e-6));
    CHECK(world->stats().catch_up_events == 0);
}

TEST_CASE("Render transforms interpolate between the last two steps", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle ball = add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));

    REQUIRE(world->update(1.5 * kFixedStep).ok());  // one step, alpha 0.5
    const auto stepped = world->step_transform(ball);
    const auto rendered = world->render_transform(ball);
    REQUIRE(stepped.has_value());
    REQUIRE(rendered.has_value());

    CHECK(stepped->position.y < 10.0);  // gravity pulled the body down during the step
    CHECK_THAT(rendered->position.y, WithinAbs(0.5 * (10.0 + stepped->position.y), 1e-6));

    // The next update completes a second step, so alpha returns to zero and the render transform is the step before it.
    REQUIRE(world->update(0.5 * kFixedStep).ok());
    const auto at_step = world->render_transform(ball);
    REQUIRE(at_step.has_value());
    CHECK_THAT(at_step->position.y, WithinAbs(stepped->position.y, 1e-6));
}

TEST_CASE("Static, dynamic, and kinematic bodies keep their motion type; kinematic bodies reach their target", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle floor = add(*world, box_at(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -5.0, 0.0), glm::vec3(20.0f, 0.5f, 20.0f)));
    const EntityHandle falling = add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));
    const EntityHandle slider = add(*world, box_at(ecs.create(), BodyMotion::Kinematic, glm::dvec3(0.0, 0.0, 0.0)));

    CHECK(world->body_count() == 3);
    CHECK(world->move_kinematic(slider, glm::dvec3(2.0, 0.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
    CHECK_FALSE(world->move_kinematic(falling, glm::dvec3(2.0, 0.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));  // not kinematic
    CHECK_FALSE(world->move_kinematic(floor, glm::dvec3(2.0, 0.0, 0.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));

    run_steps(*world, 1);
    const auto moved = world->step_transform(slider);
    REQUIRE(moved.has_value());
    CHECK_THAT(moved->position.x, WithinAbs(2.0, 1e-3));

    const auto static_pose = world->step_transform(floor);
    REQUIRE(static_pose.has_value());
    CHECK_THAT(static_pose->position.y, WithinAbs(-5.0, 1e-6));

    const auto fallen = world->step_transform(falling);
    REQUIRE(fallen.has_value());
    CHECK(fallen->position.y < 10.0);
}

TEST_CASE("Bodies refer to stable entity handles, and a stale handle never matches a later entity in the same slot", "[unit]") {
    World ecs;
    auto world = make_world();

    const EntityHandle first = ecs.create();
    const EntityHandle body_entity = add(*world, box_at(first, BodyMotion::Dynamic, glm::dvec3(0.0)));
    CHECK(body_entity == first);
    CHECK(world->has_body(first));

    // A second body for the same entity is refused, and so is a handle that is not valid.
    auto duplicate = world->add_body(box_at(first, BodyMotion::Dynamic, glm::dvec3(1.0)));
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error() == PhysicsError::InvalidEntity);
    auto invalid = world->add_body(box_at(EntityHandle{}, BodyMotion::Dynamic, glm::dvec3(0.0)));
    REQUIRE_FALSE(invalid.ok());
    CHECK(invalid.error() == PhysicsError::InvalidEntity);

    // Destroying the entity and creating another in the same slot gives a new generation, so the old handle does not match.
    CHECK(world->remove_body(first));
    CHECK_FALSE(world->remove_body(first));
    CHECK(ecs.destroy(first));
    const EntityHandle second = ecs.create();
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    add(*world, box_at(second, BodyMotion::Dynamic, glm::dvec3(0.0)));

    CHECK_FALSE(world->has_body(first));
    CHECK_FALSE(world->step_transform(first).has_value());
    CHECK(world->has_body(second));
    CHECK(world->body_count() == 1);
}

TEST_CASE("Each runtime scene owns its own world: bodies, gravity, and closure are independent", "[unit]") {
    World ecs;

    PhysicsWorldDesc gravity_desc;
    PhysicsWorldDesc weightless_desc;
    weightless_desc.gravity = glm::dvec3(0.0);
    auto scene_a = make_world(gravity_desc);
    auto scene_b = make_world(weightless_desc);

    const EntityHandle in_a = add(*scene_a, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));
    const EntityHandle in_b = add(*scene_b, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));

    CHECK_FALSE(scene_a->has_body(in_b));
    CHECK_FALSE(scene_b->has_body(in_a));

    run_steps(*scene_a, 30);
    run_steps(*scene_b, 30);
    CHECK(scene_a->step_transform(in_a)->position.y < 10.0);
    CHECK_THAT(scene_b->step_transform(in_b)->position.y, WithinAbs(10.0, 1e-6));

    // Closing one scene leaves the other running.
    scene_a->close();
    CHECK(scene_a->closed());
    CHECK_FALSE(scene_b->closed());
    REQUIRE(scene_b->update(kFixedStep).ok());
    CHECK(scene_b->stats().steps == 31);
}

TEST_CASE("Pause runs no steps and discards paused time; resuming runs only the new time", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle ball = add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));

    world->set_paused(true);
    CHECK(world->paused());
    REQUIRE(world->update(1.0).ok());  // a full second of paused time
    CHECK(world->stats().steps == 0);
    CHECK_THAT(world->step_transform(ball)->position.y, WithinAbs(10.0, 1e-6));

    world->set_paused(false);
    REQUIRE(world->update(kFixedStep).ok());
    CHECK(world->stats().steps == 1);  // the paused second did not accumulate into a burst of steps
}

TEST_CASE("The catch-up limit runs at most the configured steps, drops the rest, and counts the drop", "[unit]") {
    World ecs;
    PhysicsWorldDesc desc;
    desc.max_catch_up_steps = kDefaultMaxCatchUpSteps;
    auto world = make_world(desc);
    add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));

    REQUIRE(world->update(10.0).ok());  // ten seconds of frame time
    const auto stats = world->stats();
    CHECK(stats.steps == kDefaultMaxCatchUpSteps);
    CHECK(stats.catch_up_events == 1);
    CHECK(stats.dropped_steps > 500);
    CHECK(world->interpolation_alpha() >= 0.0);
    CHECK(world->interpolation_alpha() < 1.0);

    // The next frame of one step runs one step: the dropped time is not owed back.
    REQUIRE(world->update(kFixedStep).ok());
    CHECK(world->stats().steps == kDefaultMaxCatchUpSteps + 1);
    CHECK(world->stats().catch_up_events == 1);
}

TEST_CASE("The catch-up limit is set per world", "[unit]") {
    World ecs;
    PhysicsWorldDesc desc;
    desc.max_catch_up_steps = 2;
    auto world = make_world(desc);
    add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));

    REQUIRE(world->update(5.0 * kFixedStep).ok());
    CHECK(world->stats().steps == 2);
    CHECK(world->stats().catch_up_events == 1);
    CHECK(world->stats().dropped_steps == 3);
}

TEST_CASE("Contact and activation callbacks reach the queue with entity handles, with and without worker threads", "[unit]") {
    for (const std::uint32_t threads : {0u, 2u}) {
        CAPTURE(threads);
        World ecs;
        PhysicsWorldDesc desc;
        desc.worker_threads = threads;
        auto world = make_world(desc);

        const EntityHandle ground = add(*world, box_at(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(10.0f, 0.5f, 10.0f)));
        BodyDesc ball_desc;
        ball_desc.entity = ecs.create();
        ball_desc.shape = ShapeKind::Sphere;
        ball_desc.radius = 0.5f;
        ball_desc.position = glm::dvec3(0.0, 3.0, 0.0);
        const EntityHandle ball = add(*world, ball_desc);

        // Two seconds: the ball falls about 2.5 m, lands, and rests. The queue is drained after every step, as a game loop would.
        std::vector<PhysicsEvent> events;
        for (int i = 0; i < 120; ++i) {
            run_steps(*world, 1);
            for (PhysicsEvent& event : world->take_events()) {
                events.push_back(event);
            }
        }

        CHECK(contains_pair(events, PhysicsEventKind::ContactAdded, ball, ground));
        for (const PhysicsEvent& event : events) {
            CHECK(event.step >= 1);
            CHECK(event.step <= world->stats().steps);
        }
        CHECK(world->take_events().empty());  // nothing is delivered twice

        const auto resting = world->step_transform(ball);
        REQUIRE(resting.has_value());
        CHECK_THAT(resting->position.y, WithinAbs(0.5, 0.05));
    }
}

TEST_CASE("Body limits and invalid descriptions are reported without changing the world", "[unit]") {
    World ecs;
    PhysicsWorldDesc desc;
    desc.max_bodies = 2;
    auto world = make_world(desc);

    add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0)));
    add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(2.0)));
    auto full = world->add_body(box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(4.0)));
    REQUIRE_FALSE(full.ok());
    CHECK(full.error() == PhysicsError::BodyLimit);
    CHECK(world->body_count() == 2);

    auto flat = world->add_body(box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0), glm::vec3(0.0f, 1.0f, 1.0f)));
    REQUIRE_FALSE(flat.ok());
    CHECK(flat.error() == PhysicsError::InvalidDesc);

    PhysicsWorldDesc no_bodies;
    no_bodies.max_bodies = 0;
    auto rejected = PhysicsWorld::create(no_bodies);
    REQUIRE_FALSE(rejected.ok());
    CHECK(rejected.error() == PhysicsError::InvalidDesc);
}

TEST_CASE("Closing a world removes its bodies, drops its queue, and refuses further work", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle ball = add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));
    const EntityHandle slider = add(*world, box_at(ecs.create(), BodyMotion::Kinematic, glm::dvec3(0.0)));
    run_steps(*world, 2);

    world->close();
    CHECK(world->closed());
    CHECK(world->body_count() == 0);
    CHECK_FALSE(world->has_body(ball));
    CHECK_FALSE(world->step_transform(ball).has_value());
    CHECK_FALSE(world->render_transform(ball).has_value());
    CHECK_FALSE(world->move_kinematic(slider, glm::dvec3(1.0), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
    CHECK(world->take_events().empty());
    CHECK(world->update(kFixedStep).code == engine::core::StatusCode::InvalidState);

    auto late = world->add_body(box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0)));
    REQUIRE_FALSE(late.ok());
    CHECK(late.error() == PhysicsError::InvalidState);

    world->close();  // a second close is a no-op
    CHECK(world->closed());
}

TEST_CASE("An origin subscriber outlives its world without touching it", "[unit]") {
    World ecs;
    WorldOrigin origin;
    std::shared_ptr<engine::world::OriginSubscriber> link;
    {
        auto world = make_world();
        add(*world, box_at(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0, 10.0, 0.0)));
        link = world->origin_subscriber();
        origin.subscribe(link);
    }  // the world is destroyed here, and the origin still holds its link

    const auto shift = origin.update(glm::dvec3(5000.0, 0.0, 0.0));
    REQUIRE(shift.has_value());
    CHECK(origin.rebases() == 1);
}

TEST_CASE("Origin rebases move single-precision positions and keep the world-space transforms", "[unit]") {
    World ecs;
    WorldOrigin origin;
    auto world = make_world();
    origin.subscribe(world->origin_subscriber());

    // Far from the origin, a single-precision position has about a 0.5 mm step, so the body keeps its place only because the
    // adapter rebases it and keeps the world-space history in double.
    const glm::dvec3 start(5000.25, 20.0, -3000.5);
    const EntityHandle ball = add(*world, box_at(ecs.create(), BodyMotion::Dynamic, start));
    const EntityHandle floor = add(*world, box_at(ecs.create(), BodyMotion::Static, glm::dvec3(5000.0, -1.0, -3000.0), glm::vec3(10.0f, 0.5f, 10.0f)));

    // Teleport the origin to the body: the camera is beyond the rebase distance.
    const auto shift = origin.update(start);
    REQUIRE(shift.has_value());
    CHECK(world->origin() == glm::dvec3(start));

    const auto before_step = world->step_transform(ball);
    REQUIRE(before_step.has_value());
    CHECK(before_step->position.x == Catch::Approx(start.x).margin(1e-9));
    CHECK(before_step->position.z == Catch::Approx(start.z).margin(1e-9));

    run_steps(*world, 30);
    const auto after = world->step_transform(ball);
    REQUIRE(after.has_value());
    CHECK_THAT(after->position.x, WithinAbs(start.x, 1e-3));
    CHECK_THAT(after->position.z, WithinAbs(start.z, 1e-3));
    CHECK(after->position.y < start.y);

    CHECK_THAT(world->step_transform(floor)->position.y, WithinAbs(-1.0, 1e-6));
}

TEST_CASE("A velocity wakes a resting body, and only a dynamic body takes one", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle floor = ecs.create();
    add(*world, box_at(floor, BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(20.0f, 0.5f, 20.0f)));
    const EntityHandle ball = ecs.create();
    add(*world, box_at(ball, BodyMotion::Dynamic, glm::dvec3(0.0, 1.0, 0.0)));

    // A falling body is awake; after it lands and settles, it goes to sleep and is no longer counted as active.
    CHECK(world->active_body_count() == 1);
    run_steps(*world, 240);
    CHECK(world->active_body_count() == 0);

    CHECK(world->set_linear_velocity(ball, glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(world->active_body_count() == 1);

    CHECK_FALSE(world->set_linear_velocity(floor, glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK_FALSE(world->set_linear_velocity(ecs.create(), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK_FALSE(world->set_linear_velocity(ball, glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f)));
}

