// The character controller and joints (US-054) on the CPU: walking, slopes, steps, jumps, moving platforms, triggers, the fixed, hinge, and
// distance joints with safe removal, overlap queries, and a walking course that runs again after its cell is unloaded and reloaded.
// These need no renderer and run on every operating system.
#include "engine/ecs/world.hpp"
#include "engine/physics/character.hpp"
#include "engine/physics/collision_cells.hpp"
#include "engine/physics/physics_world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using engine::ecs::EntityHandle;
using engine::ecs::World;
using engine::physics::BodyDesc;
using engine::physics::BodyMotion;
using engine::physics::CharacterController;
using engine::physics::CharacterDesc;
using engine::physics::CollisionCellDesc;
using engine::physics::CollisionCells;
using engine::physics::ConstraintDesc;
using engine::physics::ConstraintHandle;
using engine::physics::ConstraintKind;
using engine::physics::kFixedStep;
using engine::physics::PhysicsError;
using engine::physics::PhysicsWorld;
using engine::physics::PhysicsWorldDesc;
using engine::physics::ShapeKind;

namespace {

std::unique_ptr<PhysicsWorld> make_world() {
    auto result = PhysicsWorld::create(PhysicsWorldDesc{});
    REQUIRE(result.ok());
    return std::move(result).value();
}

BodyDesc box(EntityHandle entity, BodyMotion motion, glm::dvec3 position, glm::vec3 half, glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
             bool trigger = false) {
    BodyDesc desc;
    desc.entity = entity;
    desc.motion = motion;
    desc.shape = ShapeKind::Box;
    desc.half_extents = half;
    desc.position = position;
    desc.rotation = rotation;
    desc.trigger = trigger;
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

// A box whose top face starts at `base` (x and y in meters) and rises along +x at `degrees` for `length` meters. Its top face is one
// meter below the box's centre line, so the lower end of the face sits at `base` and the box is solid beneath it.
BodyDesc ramp(EntityHandle entity, glm::vec2 base, float degrees, float length, float half_width, double z) {
    constexpr float kThickness = 1.0f;
    const float theta = glm::radians(degrees);
    const glm::vec2 along(std::cos(theta), std::sin(theta));
    const glm::vec2 up(-std::sin(theta), std::cos(theta));
    const glm::vec2 centre = base + along * (length * 0.5f) - up * (kThickness * 0.5f);
    return box(entity, BodyMotion::Static, glm::dvec3(centre.x, centre.y, z), glm::vec3(length * 0.5f, kThickness * 0.5f, half_width),
               glm::angleAxis(theta, glm::vec3(0.0f, 0.0f, 1.0f)));
}

EntityHandle add(PhysicsWorld& world, const BodyDesc& desc) {
    auto result = world.add_body(desc);
    REQUIRE(result.ok());
    return std::move(result).value();
}

ConstraintHandle join(PhysicsWorld& world, const ConstraintDesc& desc) {
    auto result = world.add_constraint(desc);
    REQUIRE(result.ok());
    return std::move(result).value();
}

void run_steps(PhysicsWorld& world, int steps) {
    for (int i = 0; i < steps; ++i) {
        REQUIRE(world.update(kFixedStep).ok());
    }
}

CharacterController make_character(glm::dvec3 feet, float step_height = 0.3f, float max_slope_degrees = 45.0f) {
    CharacterDesc desc;
    desc.position = feet;
    desc.step_height = step_height;
    desc.max_slope_degrees = max_slope_degrees;
    auto result = CharacterController::create(desc);
    REQUIRE(result.ok());
    return std::move(result).value();
}

// Walks for `seconds` at `velocity`, one fixed step per frame. The character is not moved when the time is shorter than a step.
void walk(PhysicsWorld& world, CharacterController& character, glm::vec3 velocity, double seconds) {
    const int frames = static_cast<int>(std::lround(seconds / kFixedStep));
    for (int i = 0; i < frames; ++i) {
        REQUIRE(world.update(kFixedStep).ok());
        REQUIRE(character.update(world, velocity, false, kFixedStep).ok());
    }
}

// The height of the feet on a slope `run` metres above its foot: the surface height, plus the rounded base's offset from the centre
// (r / cos - r) and the 2 cm skin the feet keep above the surface.
double surface_feet(double run, double degrees) {
    const double slope = std::tan(glm::radians(degrees));
    const double base_offset = 0.35 * (1.0 / std::cos(glm::radians(degrees)) - 1.0);
    return run * slope + base_offset + 0.02;
}

bool contains(const std::vector<EntityHandle>& entities, EntityHandle entity) {
    return std::find(entities.begin(), entities.end(), entity) != entities.end();
}

}  // namespace

TEST_CASE("The character walks a flat floor at its speed, stops when the input stops, and stays grounded", "[unit]") {
    World ecs;
    auto world = make_world();
    add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(20.0f, 0.5f, 5.0f)));

    auto character = make_character(glm::dvec3(-5.0, 0.0, 0.0));
    walk(*world, character, glm::vec3(2.0f, 0.0f, 0.0f), 1.0);
    CHECK_THAT(character.position().x, WithinAbs(-3.0, 0.02));
    CHECK_THAT(character.position().y, WithinAbs(0.0, 0.03));  // the feet rest one skin, 2 cm, above the floor
    CHECK(character.grounded());

    walk(*world, character, glm::vec3(0.0f), 0.5);
    CHECK_THAT(character.position().x, WithinAbs(-3.0, 0.02));
    CHECK(character.grounded());
}

TEST_CASE("A jump rises to its apex under gravity and lands on the floor again", "[unit]") {
    World ecs;
    auto world = make_world();
    add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(20.0f, 0.5f, 5.0f)));

    auto character = make_character(glm::dvec3(0.0));
    walk(*world, character, glm::vec3(0.0f), 0.2);
    REQUIRE(character.grounded());

    // A jump at 5 m/s rises v^2 / 2g, about 1.27 m; the request is taken on the first frame only.
    double apex = 0.0;
    for (int frame = 0; frame < 120; ++frame) {
        REQUIRE(world->update(kFixedStep).ok());
        REQUIRE(character.update(*world, glm::vec3(0.0f), frame == 0, kFixedStep).ok());
        apex = std::max(apex, character.position().y);
    }
    CHECK_THAT(apex, WithinAbs(5.0 * 5.0 / (2.0 * 9.81), 0.1));
    CHECK(character.grounded());
    CHECK_THAT(character.position().y, WithinAbs(0.0, 0.03));
}

TEST_CASE("The character climbs a slope inside its limit, and a steeper slope stops it", "[unit]") {
    World ecs;
    auto world = make_world();
    add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(20.0f, 0.5f, 5.0f)));
    // A 30-degree ramp starting at x = 0 is walkable; a 60-degree ramp at z = 4 is not.
    add(*world, ramp(ecs.create(), glm::vec2(0.0f, 0.0f), 30.0f, 6.0f, 1.0f, 0.0));
    add(*world, ramp(ecs.create(), glm::vec2(0.0f, 0.0f), 60.0f, 4.0f, 1.0f, 4.0));

    auto climber = make_character(glm::dvec3(-3.0, 0.0, 0.0));
    walk(*world, climber, glm::vec3(1.5f, 0.0f, 0.0f), 4.0);
    // At x = 3 the 30-degree ramp is 3 * tan(30) = 1.73 m high. The rounded base touches the slope off the centre, so the feet stand a
    // little above the surface beneath them.
    CHECK_THAT(climber.position().x, WithinAbs(3.0, 0.05));
    CHECK_THAT(climber.position().y, WithinAbs(surface_feet(3.0, 30.0), 0.03));
    CHECK(climber.grounded());

    // The 60-degree face stops the rounded base of the capsule 0.35 m from its surface. Here that point is x = -0.19, because the
    // base sits above the ramp's lower corner, and the character stays on the floor beside it.
    auto wall = make_character(glm::dvec3(-3.0, 0.0, 4.0));
    walk(*world, wall, glm::vec3(1.5f, 0.0f, 0.0f), 3.0);
    CHECK_THAT(wall.position().x, WithinAbs(-0.19, 0.03));
    CHECK_THAT(wall.position().y, WithinAbs(0.0, 0.03));
}

TEST_CASE("Stairs and ledges: steps within the step height are climbed, and a higher ledge stops the character", "[unit]") {
    World ecs;
    auto world = make_world();
    add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(20.0f, 0.5f, 5.0f)));
    // Five 0.2 m risers, each block running to x = 10, so the top rises to 1.0 m.
    for (int k = 0; k < 5; ++k) {
        const float top = 0.2f * static_cast<float>(k + 1);
        const double left = 2.0 + 0.5 * k;
        add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3((left + 10.0) * 0.5, top * 0.5, -3.0),
                        glm::vec3(static_cast<float>(10.0 - left) * 0.5f, top * 0.5f, 1.0f)));
    }
    // A 0.5 m ledge is higher than the 0.3 m step, so it stops the character at its face.
    add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(6.0, 0.25, 4.0), glm::vec3(4.0f, 0.25f, 1.0f)));

    auto climber = make_character(glm::dvec3(0.0, 0.0, -3.0));
    walk(*world, climber, glm::vec3(1.5f, 0.0f, 0.0f), 5.5);
    // Each riser adds up to half a radius of reach, so the climber is at least where its walk put it, and not past the top's edge.
    CHECK(climber.position().x > 8.2);
    CHECK(climber.position().x < 9.5);
    CHECK_THAT(climber.position().y, WithinAbs(1.0, 0.05));
    CHECK(climber.grounded());

    auto ledge = make_character(glm::dvec3(0.0, 0.0, 4.0));
    walk(*world, ledge, glm::vec3(2.0f, 0.0f, 0.0f), 3.0);
    CHECK(ledge.position().x < 2.0);
    CHECK_THAT(ledge.position().y, WithinAbs(0.0, 0.05));
}

TEST_CASE("A trigger is reported by overlaps and never stops the character", "[unit]") {
    World ecs;
    auto world = make_world();
    add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(20.0f, 0.5f, 5.0f)));
    const EntityHandle trigger = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(5.0, 1.0, 0.0), glm::vec3(0.5f, 1.0f, 1.0f),
                                                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f), true));

    auto character = make_character(glm::dvec3(0.0));
    bool seen = false;
    for (int frame = 0; frame < 240; ++frame) {
        REQUIRE(world->update(kFixedStep).ok());
        REQUIRE(character.update(*world, glm::vec3(2.0f, 0.0f, 0.0f), false, kFixedStep).ok());
        seen = seen || contains(character.overlaps(*world), trigger);
    }
    CHECK(seen);
    CHECK_THAT(character.position().x, WithinAbs(8.0, 0.05));
}

TEST_CASE("A character standing on a moving platform is carried by it", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle platform = add(*world, box(ecs.create(), BodyMotion::Kinematic, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(3.0f, 0.5f, 3.0f)));

    auto character = make_character(glm::dvec3(0.0));
    glm::dvec3 target(0.0, -0.5, 0.0);
    for (int frame = 0; frame < 120; ++frame) {
        target.x += 1.0 * kFixedStep;  // the platform moves at 1 m/s and the character does not walk
        REQUIRE(world->move_kinematic(platform, target, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
        REQUIRE(world->update(kFixedStep).ok());
        REQUIRE(character.update(*world, glm::vec3(0.0f), false, kFixedStep).ok());
    }
    CHECK_THAT(character.position().x, WithinAbs(2.0, 0.05));
    CHECK_THAT(character.position().y, WithinAbs(0.0, 0.03));
    CHECK(character.grounded());
    CHECK(character.ground() == platform);
}

TEST_CASE("A fixed joint holds a body at its anchor while gravity acts; a body without one falls", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle post = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, 5.0, 0.0), glm::vec3(0.2f)));
    const EntityHandle held = add(*world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(1.0, 5.0, 0.0), glm::vec3(0.5f)));
    const EntityHandle loose = add(*world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(-1.0, 5.0, 0.0), glm::vec3(0.5f)));

    ConstraintDesc weld;
    weld.kind = ConstraintKind::Fixed;
    weld.body_a = post;
    weld.body_b = held;
    weld.anchor_a = glm::vec3(1.0f, 0.0f, 0.0f);  // the point (1, 5, 0) on the post
    weld.anchor_b = glm::vec3(0.0f);              // the held body's centre
    CHECK(join(*world, weld).valid());
    CHECK(world->constraint_count() == 1);

    run_steps(*world, 60);
    const auto held_pose = world->step_transform(held);
    REQUIRE(held_pose.has_value());
    CHECK_THAT(held_pose->position.x, WithinAbs(1.0, 0.01));
    CHECK_THAT(held_pose->position.y, WithinAbs(5.0, 0.01));
    CHECK(world->step_transform(loose)->position.y < 4.0);
}

TEST_CASE("A hinge keeps its anchor on the pivot and lets the body swing about it", "[unit]") {
    World ecs;
    auto world = make_world();
    const glm::dvec3 pivot(0.0, 4.0, 0.0);
    const glm::vec3 top(0.0f, 0.5f, 0.0f);  // the body's point on the pivot, in its local frame
    const glm::quat tilt = glm::angleAxis(0.5f, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::dvec3 centre = pivot - glm::dvec3(tilt * top);
    // The post sits a metre above the pivot, so the swinging body never touches it.
    const EntityHandle post = add(*world, box(ecs.create(), BodyMotion::Static, pivot + glm::dvec3(0.0, 1.0, 0.0), glm::vec3(0.1f)));
    const EntityHandle bob = add(*world, box(ecs.create(), BodyMotion::Dynamic, centre, glm::vec3(0.25f, 0.5f, 0.25f), tilt));

    ConstraintDesc hinge;
    hinge.kind = ConstraintKind::Hinge;
    hinge.body_a = post;
    hinge.body_b = bob;
    hinge.anchor_a = glm::vec3(0.0f, -1.0f, 0.0f);
    hinge.anchor_b = top;
    hinge.axis_a = glm::vec3(0.0f, 0.0f, 1.0f);
    hinge.axis_b = glm::vec3(0.0f, 0.0f, 1.0f);
    hinge.normal_a = glm::vec3(1.0f, 0.0f, 0.0f);
    hinge.normal_b = glm::vec3(1.0f, 0.0f, 0.0f);
    join(*world, hinge);

    bool swung = false;
    for (int frame = 0; frame < 120; ++frame) {
        run_steps(*world, 1);
        const auto pose = world->step_transform(bob);
        REQUIRE(pose.has_value());
        const glm::dvec3 hinge_point = pose->position + glm::dvec3(pose->rotation * top);
        CHECK_THAT(glm::length(hinge_point - pivot), WithinAbs(0.0, 0.02));
        swung = swung || glm::length(pose->position - centre) > 0.1;
    }
    CHECK(swung);
}

TEST_CASE("A distance joint keeps its anchors between its limits while the body swings", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle post = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, 10.0, 0.0), glm::vec3(0.1f)));
    // Two metres out to the side, so the body starts at the far limit and swings.
    const EntityHandle bob = add(*world, sphere(ecs.create(), BodyMotion::Dynamic, glm::dvec3(2.0, 10.0, 0.0), 0.2f));

    ConstraintDesc rope;
    rope.kind = ConstraintKind::Distance;
    rope.body_a = post;
    rope.body_b = bob;
    rope.min_distance = 0.0f;
    rope.max_distance = 2.0f;
    join(*world, rope);

    double farthest = 0.0;
    for (int frame = 0; frame < 120; ++frame) {
        run_steps(*world, 1);
        const glm::dvec3 offset = world->step_transform(bob)->position - glm::dvec3(0.0, 10.0, 0.0);
        farthest = std::max(farthest, glm::length(offset));
    }
    CHECK(farthest < 2.05);
    CHECK(world->step_transform(bob)->position.y < 9.5);
}

TEST_CASE("Joints with invalid anchors, axes, limits, or bodies are refused without changing the world", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle a = add(*world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0), glm::vec3(0.5f)));
    const EntityHandle b = add(*world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(2.0, 0.0, 0.0), glm::vec3(0.5f)));

    ConstraintDesc good;
    good.body_a = a;
    good.body_b = b;
    CHECK(world->add_constraint(good).ok());
    CHECK(world->constraint_count() == 1);

    ConstraintDesc same = good;
    same.body_b = a;
    CHECK(world->add_constraint(same).error() == PhysicsError::InvalidDesc);

    ConstraintDesc skewed = good;
    skewed.normal_a = glm::vec3(0.0f, 1.0f, 0.0f);  // parallel to the default axis
    CHECK(world->add_constraint(skewed).error() == PhysicsError::InvalidDesc);

    ConstraintDesc hinge = good;
    hinge.kind = ConstraintKind::Hinge;
    hinge.max_limit = 4.0f;  // beyond pi
    CHECK(world->add_constraint(hinge).error() == PhysicsError::InvalidDesc);

    ConstraintDesc distance = good;
    distance.kind = ConstraintKind::Distance;
    distance.min_distance = 3.0f;
    distance.max_distance = 2.0f;
    CHECK(world->add_constraint(distance).error() == PhysicsError::InvalidDesc);

    ConstraintDesc missing = good;
    missing.body_b = ecs.create();
    CHECK(world->add_constraint(missing).error() == PhysicsError::InvalidEntity);
    CHECK(world->constraint_count() == 1);

    world->close();
    CHECK(world->add_constraint(good).error() == PhysicsError::InvalidState);
}

TEST_CASE("Removing a body removes its joints, a joint is removed once, and a stale handle matches no later joint", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle a = add(*world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(0.0), glm::vec3(0.5f)));
    const EntityHandle b = add(*world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(2.0, 0.0, 0.0), glm::vec3(0.5f)));
    const EntityHandle c = add(*world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(4.0, 0.0, 0.0), glm::vec3(0.5f)));

    ConstraintDesc first;
    first.body_a = a;
    first.body_b = b;
    const ConstraintHandle joined_ab = join(*world, first);
    ConstraintDesc second;
    second.body_a = b;
    second.body_b = c;
    const ConstraintHandle joined_bc = join(*world, second);
    CHECK(world->constraint_count() == 2);

    // Removing a body removes every joint that names it, and the other joint stays.
    CHECK(world->remove_body(a));
    CHECK(world->constraint_count() == 1);
    CHECK_FALSE(world->remove_constraint(joined_ab));
    CHECK(world->remove_constraint(joined_bc));
    CHECK_FALSE(world->remove_constraint(joined_bc));
    CHECK(world->constraint_count() == 0);

    // A new joint gets a new handle, so the old one still matches nothing.
    const ConstraintHandle fresh = join(*world, second);
    CHECK_FALSE(fresh == joined_bc);
    CHECK_FALSE(world->remove_constraint(joined_bc));
    run_steps(*world, 10);
    CHECK(world->remove_constraint(fresh));
}

TEST_CASE("An overlap query returns the bodies a shape overlaps, with triggers only when the filter asks for them", "[unit]") {
    World ecs;
    auto world = make_world();
    const EntityHandle solid = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0), glm::vec3(1.0f)));
    const EntityHandle trigger = add(*world, box(ecs.create(), BodyMotion::Static, glm::dvec3(5.0, 0.0, 0.0), glm::vec3(1.0f),
                                                 glm::quat(1.0f, 0.0f, 0.0f, 0.0f), true));

    const BodyDesc on_solid = sphere(EntityHandle{}, BodyMotion::Kinematic, glm::dvec3(0.5, 0.5, 0.0), 0.5f);
    const std::vector<EntityHandle> hit_solid = world->overlaps(on_solid);
    CHECK(contains(hit_solid, solid));
    CHECK_FALSE(contains(hit_solid, trigger));

    const BodyDesc on_trigger = sphere(EntityHandle{}, BodyMotion::Kinematic, glm::dvec3(5.2, 0.0, 0.0), 0.5f);
    CHECK(contains(world->overlaps(on_trigger), trigger));
    engine::physics::QueryFilter solids_only;
    solids_only.include_triggers = false;
    CHECK_FALSE(contains(world->overlaps(on_trigger, solids_only), trigger));

    // A shape that only touches a body does not overlap it.
    CHECK(world->overlaps(sphere(EntityHandle{}, BodyMotion::Kinematic, glm::dvec3(2.6, 0.0, 0.0), 0.5f)).empty());
    CHECK(world->overlaps(sphere(EntityHandle{}, BodyMotion::Kinematic, glm::dvec3(20.0, 0.0, 0.0), 0.5f)).empty());
}

TEST_CASE("A character description that cannot be walked is refused, and a closed world refuses the update", "[unit]") {
    CharacterDesc bad;
    bad.radius = 0.0f;
    CHECK(CharacterController::create(bad).error() == PhysicsError::InvalidDesc);

    CharacterDesc vertical;
    vertical.max_slope_degrees = 90.0f;
    CHECK(CharacterController::create(vertical).error() == PhysicsError::InvalidDesc);

    CharacterDesc negative_step;
    negative_step.step_height = -0.1f;
    CHECK(CharacterController::create(negative_step).error() == PhysicsError::InvalidDesc);

    auto world = make_world();
    auto character = make_character(glm::dvec3(0.0));
    world->close();
    const auto status = character.update(*world, glm::vec3(0.0f), false, kFixedStep);
    CHECK(status.code == engine::core::StatusCode::InvalidState);
}

// The walking course: a 30-degree ramp to a plateau, a stair of five 0.2 m risers with a trigger at its top, and a 60-degree ramp that
// must stop the character. The course lies in cell 1, which spans x in [-12, 12]. A fixed joint holds a free body just outside that span,
// welded to a post inside it, so the joint's removal with the cell is visible.
namespace {

struct Course {
    EntityHandle trigger;
    EntityHandle post;
};

struct CourseResult {
    double ramp_mid_y = 0.0;   // feet halfway up the 30-degree ramp
    double plateau_y = 0.0;    // feet on the plateau
    double stairs_x = 0.0;     // feet at the top of the stair
    double stairs_y = 0.0;
    bool trigger_seen = false;
    double cliff_x = 0.0;      // feet at the 60-degree ramp
    double cliff_y = 0.0;
};

CollisionCellDesc course_cell(World& ecs, Course& course) {
    CollisionCellDesc cell;
    cell.id = 1;
    cell.min = glm::dvec3(-12.0, -10.0, -6.0);
    cell.max = glm::dvec3(12.0, 10.0, 6.0);
    cell.bodies.push_back(box(ecs.create(), BodyMotion::Static, glm::dvec3(0.0, -0.5, 0.0), glm::vec3(12.0f, 0.5f, 6.0f)));
    cell.bodies.push_back(ramp(ecs.create(), glm::vec2(-8.0f, 0.0f), 30.0f, 3.0f, 1.0f, 0.0));
    cell.bodies.push_back(box(ecs.create(), BodyMotion::Static, glm::dvec3(-3.7, 0.75, 0.0), glm::vec3(1.7f, 0.75f, 1.0f)));
    for (int k = 0; k < 5; ++k) {
        const float top = 0.2f * static_cast<float>(k + 1);
        const double left = 2.0 + 0.5 * k;
        cell.bodies.push_back(box(ecs.create(), BodyMotion::Static, glm::dvec3((left + 10.0) * 0.5, top * 0.5, -3.0),
                                  glm::vec3(static_cast<float>(10.0 - left) * 0.5f, top * 0.5f, 1.0f)));
    }
    course.trigger = ecs.create();
    cell.bodies.push_back(box(course.trigger, BodyMotion::Static, glm::dvec3(9.0, 1.5, -3.0), glm::vec3(0.4f, 1.5f, 1.0f),
                              glm::quat(1.0f, 0.0f, 0.0f, 0.0f), true));
    cell.bodies.push_back(ramp(ecs.create(), glm::vec2(-8.0f, 0.0f), 60.0f, 4.0f, 1.0f, 4.0));
    course.post = ecs.create();
    cell.bodies.push_back(box(course.post, BodyMotion::Static, glm::dvec3(12.0, 1.0, -5.0), glm::vec3(0.5f)));
    return cell;
}

// Walks the three lanes. Each lane has its own character, so a stop in one does not change the others.
CourseResult walk_course(PhysicsWorld& world, const Course& course) {
    CourseResult result;

    auto ramp_walker = make_character(glm::dvec3(-10.0, 0.0, 0.0));
    walk(world, ramp_walker, glm::vec3(1.5f, 0.0f, 0.0f), 2.0);
    result.ramp_mid_y = ramp_walker.position().y;
    walk(world, ramp_walker, glm::vec3(1.5f, 0.0f, 0.0f), 1.5);
    result.plateau_y = ramp_walker.position().y;

    auto climber = make_character(glm::dvec3(0.0, 0.0, -3.0));
    for (int frame = 0; frame < 360; ++frame) {  // 6 s at 1.5 m/s: the climber ends at x = 9, over the trigger
        REQUIRE(world.update(kFixedStep).ok());
        REQUIRE(climber.update(world, glm::vec3(1.5f, 0.0f, 0.0f), false, kFixedStep).ok());
        result.trigger_seen = result.trigger_seen || contains(climber.overlaps(world), course.trigger);
    }
    result.stairs_x = climber.position().x;
    result.stairs_y = climber.position().y;

    auto cliff = make_character(glm::dvec3(-10.0, 0.0, 4.0));
    walk(world, cliff, glm::vec3(1.5f, 0.0f, 0.0f), 3.0);
    result.cliff_x = cliff.position().x;
    result.cliff_y = cliff.position().y;
    return result;
}

// Adds a free body welded to the course's post. The body sits at x = 13.5, beyond the cell's footprint plus its unload margin.
void weld_to_post(World& ecs, PhysicsWorld& world, EntityHandle post, EntityHandle& body) {
    body = add(world, box(ecs.create(), BodyMotion::Dynamic, glm::dvec3(13.5, 1.0, -5.0), glm::vec3(0.5f)));
    ConstraintDesc weld;
    weld.kind = ConstraintKind::Fixed;
    weld.body_a = post;
    weld.body_b = body;
    weld.anchor_a = glm::vec3(0.0f);
    weld.anchor_b = glm::vec3(-1.5f, 0.0f, 0.0f);  // the point (12, 1, -5), the post's centre
    join(world, weld);
}

}  // namespace

TEST_CASE("Stairs, slopes, triggers, and joints behave the same after their cell unloads and reloads", "[unit]") {
    World ecs;
    auto world = make_world();
    CollisionCells cells(*world, 1.0);

    // First load: the course is walked, and the welded body holds its place.
    Course course;
    auto loaded = cells.load(course_cell(ecs, course));
    REQUIRE(loaded.ok());
    EntityHandle bob;
    weld_to_post(ecs, *world, course.post, bob);
    CHECK(world->constraint_count() == 1);
    run_steps(*world, 60);
    CHECK_THAT(world->step_transform(bob)->position.y, WithinAbs(1.0, 0.02));

    const CourseResult first = walk_course(*world, course);
    CHECK_THAT(first.ramp_mid_y, WithinAbs(surface_feet(1.0, 30.0), 0.03));  // one metre up the ramp from its foot at x = -8
    CHECK_THAT(first.plateau_y, WithinAbs(1.5, 0.05));
    CHECK_THAT(first.stairs_y, WithinAbs(1.0, 0.05));
    CHECK(first.trigger_seen);
    CHECK(first.cliff_x < -7.8);
    CHECK_THAT(first.cliff_y, WithinAbs(0.0, 0.05));

    // Unload: the post leaves with the cell, so the joint is removed with it and the welded body falls.
    REQUIRE(cells.unload(1));
    CHECK_FALSE(cells.loaded(1));
    CHECK_FALSE(world->has_body(course.post));
    CHECK_FALSE(world->has_body(course.trigger));
    CHECK(world->constraint_count() == 0);
    run_steps(*world, 60);
    CHECK(world->step_transform(bob)->position.y < 0.0);

    // With the floor gone, a character dropped on the lane falls past it.
    auto dropped = make_character(glm::dvec3(-10.0, 0.5, 0.0));
    walk(*world, dropped, glm::vec3(0.0f), 1.0);
    CHECK(dropped.position().y < -1.0);

    // Reload: the same course, with new entities, walks to the same places, and the joint is made again.
    REQUIRE(world->remove_body(bob));
    Course reloaded;
    auto again = cells.load(course_cell(ecs, reloaded));
    REQUIRE(again.ok());
    EntityHandle new_bob;
    weld_to_post(ecs, *world, reloaded.post, new_bob);
    CHECK(world->constraint_count() == 1);
    run_steps(*world, 60);
    CHECK_THAT(world->step_transform(new_bob)->position.y, WithinAbs(1.0, 0.02));

    const CourseResult second = walk_course(*world, reloaded);
    CHECK_THAT(second.ramp_mid_y, WithinAbs(first.ramp_mid_y, 0.02));
    CHECK_THAT(second.plateau_y, WithinAbs(first.plateau_y, 0.02));
    CHECK_THAT(second.stairs_x, WithinAbs(first.stairs_x, 0.05));
    CHECK_THAT(second.stairs_y, WithinAbs(first.stairs_y, 0.02));
    CHECK(second.trigger_seen == first.trigger_seen);
    CHECK_THAT(second.cliff_x, WithinAbs(first.cliff_x, 0.05));
    CHECK_THAT(second.cliff_y, WithinAbs(first.cliff_y, 0.02));
}
