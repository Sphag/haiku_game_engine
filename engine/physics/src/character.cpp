#include "engine/physics/character.hpp"

#include "engine/core/assert.hpp"

#include <algorithm>
#include <cmath>

namespace engine::physics {

namespace {

// The gap kept between the capsule and any surface. A cast stops this far short of contact, so the next cast starts clear of it.
constexpr float kSkin = 0.02f;
// Slide passes per move. Each pass ends on a contact, so a move past a corner needs two or three.
constexpr int kMaxSlidePasses = 4;
// Moves shorter than this are dropped.
constexpr float kMinMove = 1e-5f;
constexpr float kMaxFallSpeed = 50.0f;  // m/s

bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The capsule as a query shape: its bottom sits at `feet`, and its centre is one radius and half-height above.
BodyDesc capsule_at(const glm::dvec3& feet, float radius, float half_height) {
    BodyDesc shape;
    shape.motion = BodyMotion::Kinematic;
    shape.shape = ShapeKind::Capsule;
    shape.radius = radius;
    shape.half_height = half_height;
    shape.position = feet + glm::dvec3(0.0, static_cast<double>(radius) + half_height, 0.0);
    return shape;
}

}  // namespace

core::Result<CharacterController, PhysicsError> CharacterController::create(const CharacterDesc& desc) {
    const bool valid = std::isfinite(desc.radius) && desc.radius > 0.0f && std::isfinite(desc.half_height) && desc.half_height > 0.0f &&
                       std::isfinite(desc.max_slope_degrees) && desc.max_slope_degrees > 0.0f && desc.max_slope_degrees < 90.0f &&
                       std::isfinite(desc.step_height) && desc.step_height >= 0.0f && std::isfinite(desc.jump_speed) &&
                       desc.jump_speed >= 0.0f && finite(desc.gravity) && finite(desc.position);
    if (!valid) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    return core::success(CharacterController(desc));
}

CharacterController::CharacterController(const CharacterDesc& desc)
    : radius_(desc.radius),
      half_height_(desc.half_height),
      max_slope_cos_(std::cos(glm::radians(desc.max_slope_degrees))),
      step_height_(desc.step_height),
      jump_speed_(desc.jump_speed),
      gravity_(desc.gravity),
      filter_{desc.layers, false, ecs::EntityHandle{}},
      feet_(desc.position + glm::dvec3(0.0, kSkin, 0.0)) {}

core::Status CharacterController::update(const PhysicsWorld& world, const glm::vec3& horizontal_velocity, bool jump,
                                         double frame_seconds) {
    if (world.closed()) {
        return {core::StatusCode::InvalidState};
    }
    ENGINE_ASSERT(std::isfinite(frame_seconds) && frame_seconds >= 0.0, "frame time must be finite and not negative");
    ENGINE_ASSERT(finite(horizontal_velocity), "walk velocity must be finite");
    const float dt = static_cast<float>(frame_seconds);

    carry(world);

    velocity_.x = horizontal_velocity.x;
    velocity_.z = horizontal_velocity.z;
    if (jump && grounded_) {
        velocity_.y = jump_speed_;
        grounded_ = false;
        ground_ = ecs::EntityHandle{};
    } else if (grounded_) {
        velocity_.y = 0.0f;
    } else {
        velocity_.y = std::max(velocity_.y + gravity_.y * dt, -kMaxFallSpeed);
    }

    // Walk first, then move vertically. A step is tried only for the walk, and only while the character was on the ground.
    const Slide walk = slide(world, feet_, glm::vec3(velocity_.x * dt, 0.0f, velocity_.z * dt), true);
    feet_ = walk.feet;
    const Slide vertical = slide(world, feet_, glm::vec3(0.0f, velocity_.y * dt, 0.0f), false);
    feet_ = vertical.feet;
    if (vertical.blocked && (velocity_.y > 0.0f || vertical.blocked_by_walkable)) {
        velocity_.y = 0.0f;
    }

    // A rising character has left the ground. Otherwise look for a floor within the step height below, so the character stays on
    // a slope or a lower step rather than bouncing off it.
    if (velocity_.y <= 0.0f) {
        settle(world, step_height_ + kSkin - velocity_.y * dt);
    } else {
        grounded_ = false;
        ground_ = ecs::EntityHandle{};
    }
    return {};
}

void CharacterController::teleport(const glm::dvec3& position) {
    feet_ = position + glm::dvec3(0.0, kSkin, 0.0);
    velocity_ = glm::vec3(0.0f);
    grounded_ = false;
    ground_ = ecs::EntityHandle{};
}

glm::dvec3 CharacterController::position() const noexcept {
    return feet_;
}

glm::vec3 CharacterController::velocity() const noexcept {
    return velocity_;
}

bool CharacterController::grounded() const noexcept {
    return grounded_;
}

ecs::EntityHandle CharacterController::ground() const noexcept {
    return ground_;
}

std::vector<ecs::EntityHandle> CharacterController::overlaps(const PhysicsWorld& world) const {
    // Triggers count here, unlike in the moves above, where they never block.
    const QueryFilter filter{filter_.layers, true, ecs::EntityHandle{}};
    return world.overlaps(capsule_at(feet_, radius_, half_height_), filter);
}

std::optional<QueryHit> CharacterController::sweep(const PhysicsWorld& world, const glm::dvec3& feet, const glm::vec3& direction,
                                                   float distance) const {
    return world.shape_cast(capsule_at(feet, radius_, half_height_), direction, distance, filter_);
}

bool CharacterController::walkable(const glm::vec3& normal) const noexcept {
    return normal.y >= max_slope_cos_;
}

CharacterController::Slide CharacterController::slide(const PhysicsWorld& world, const glm::dvec3& feet, const glm::vec3& motion,
                                                      bool allow_step) const {
    Slide result{};
    result.feet = feet;
    glm::vec3 remaining = motion;
    for (int pass = 0; pass < kMaxSlidePasses && glm::length(remaining) > kMinMove; ++pass) {
        const float length = glm::length(remaining);
        const glm::vec3 direction = remaining / length;
        const std::optional<QueryHit> hit = sweep(world, result.feet, direction, length + kSkin);
        if (!hit) {
            result.feet += glm::dvec3(remaining);
            return result;
        }

        result.blocked = true;
        result.blocked_by_walkable = result.blocked_by_walkable || walkable(hit->normal);
        const float travel = std::max(static_cast<float>(hit->distance) - kSkin, 0.0f);
        result.feet += glm::dvec3(direction * travel);
        remaining -= direction * travel;

        // A wall in the way of a walk on the ground may be a ledge. Climb it when the step over it is clear.
        if (allow_step && grounded_ && !walkable(hit->normal)) {
            const glm::vec3 horizontal(remaining.x, 0.0f, remaining.z);
            if (glm::length(horizontal) > kMinMove) {
                if (const std::optional<glm::dvec3> stepped = try_step(world, result.feet, horizontal)) {
                    result.feet = *stepped;
                    return result;
                }
            }
        }

        // Whatever is left of the motion slides along the surface. A walk over a walkable surface keeps its horizontal speed: the rest of
        // it is lifted onto the slope, not projected onto it, which would shorten the walk by cos(slope). A steep surface never carries
        // the character up, so its projected motion is kept level or downward.
        if (walkable(hit->normal) && remaining.y == 0.0f) {
            remaining.y = -(hit->normal.x * remaining.x + hit->normal.z * remaining.z) / hit->normal.y;
        } else {
            remaining -= hit->normal * glm::dot(remaining, hit->normal);
            if (!walkable(hit->normal) && remaining.y > 0.0f) {
                remaining.y = 0.0f;
            }
        }
    }
    return result;
}

std::optional<glm::dvec3> CharacterController::try_step(const PhysicsWorld& world, const glm::dvec3& feet,
                                                        const glm::vec3& horizontal) const {
    // Rise as far as the step allows, or as far as a ceiling lets the character.
    // Only a surface facing down is a ceiling. A wall brushed at the capsule's side does not limit the rise.
    const std::optional<QueryHit> ceiling = sweep(world, feet, glm::vec3(0.0f, 1.0f, 0.0f), step_height_ + kSkin);
    const bool overhead = ceiling && ceiling->normal.y < 0.0f;
    const float rise = overhead ? std::max(static_cast<float>(ceiling->distance) - kSkin, 0.0f) : step_height_;
    const glm::dvec3 raised = feet + glm::dvec3(0.0, rise, 0.0);

    // Cross the step without stepping again. Any contact on the way means the step is not clear. The rounded base rests on the tread
    // only once its centre is past the riser's edge, which a frame's motion may not reach, so the step reaches half a radius further.
    const glm::vec3 reach = horizontal + glm::normalize(horizontal) * (radius_ * 0.5f);
    const Slide across = slide(world, raised, reach, false);
    if (across.blocked) {
        return std::nullopt;
    }

    // Drop onto the surface below. It must be a floor no higher than the step.
    const std::optional<QueryHit> support = sweep(world, across.feet, glm::vec3(0.0f, -1.0f, 0.0f), rise + kSkin);
    if (!support || !walkable(support->normal)) {
        return std::nullopt;
    }
    const glm::dvec3 landed = across.feet - glm::dvec3(0.0, static_cast<float>(support->distance) - kSkin, 0.0);
    if (landed.y > feet.y + step_height_ + kSkin) {
        return std::nullopt;
    }
    return landed;
}

// Moves the feet with the platform the character stands on. The offset from the platform's last pose is kept, including its rotation.
void CharacterController::carry(const PhysicsWorld& world) {
    if (!grounded_ || !ground_.valid()) {
        return;
    }
    const std::optional<PhysicsTransform> pose = world.step_transform(ground_);
    if (!pose) {
        ground_ = ecs::EntityHandle{};
        return;
    }
    const glm::dquat turn = glm::dquat(pose->rotation) * glm::inverse(glm::dquat(ground_pose_.rotation));
    feet_ = pose->position + turn * (feet_ - ground_pose_.position);
    ground_pose_ = *pose;
}

// Looks for a walkable surface within `reach` below the feet. A walkable surface is taken as the ground, and the feet rest a skin above it.
void CharacterController::settle(const PhysicsWorld& world, float reach) {
    const std::optional<QueryHit> hit = sweep(world, feet_, glm::vec3(0.0f, -1.0f, 0.0f), reach);
    if (!hit || !walkable(hit->normal)) {
        grounded_ = false;
        ground_ = ecs::EntityHandle{};
        return;
    }
    // A contact closer than the skin is lifted to it, so the feet never rest on the surface, where a horizontal move would stick.
    feet_.y -= static_cast<float>(hit->distance) - kSkin;
    grounded_ = true;
    velocity_.y = 0.0f;
    ground_ = hit->entity;
    if (ground_.valid()) {
        if (const std::optional<PhysicsTransform> pose = world.step_transform(ground_)) {
            ground_pose_ = *pose;
        } else {
            ground_ = ecs::EntityHandle{};
        }
    }
}

}  // namespace engine::physics
