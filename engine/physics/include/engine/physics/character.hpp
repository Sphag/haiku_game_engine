#pragma once

// Kinematic character controller (US-054). The character is a capsule that is not a body. It moves by shape casts against the world's
// colliders, so it neither pushes dynamic bodies nor is pushed by them. It walks surfaces up to max_slope_degrees, climbs ledges up to
// step_height, jumps under its gravity, and rides the platform it stands on. Triggers never block it, and it is found by overlaps().
// Its state is in world meters: position() is the bottom of the capsule.

#include "engine/core/result.hpp"
#include "engine/core/status.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/physics/physics_world.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace engine::physics {

struct CharacterDesc {
    glm::dvec3 position{0.0};               // world meters: the bottom of the capsule
    float radius = 0.35f;                   // meters, positive
    float half_height = 0.55f;              // half the length of the cylinder between the caps, positive
    float max_slope_degrees = 45.0f;        // surfaces inclined more than this block the character; in (0, 90)
    float step_height = 0.3f;               // ledges up to this height are climbed without a jump; not negative
    float jump_speed = 5.0f;                // upward m/s at a jump; not negative
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};  // m/s^2
    std::uint32_t layers = 0xFFFFFFFFu;     // collision layers the character collides with
};

class CharacterController {
public:
    // Fails with InvalidDesc for a capsule that is not positive, a slope outside (0, 90), a negative step or jump, or a non-finite value.
    [[nodiscard]] static core::Result<CharacterController, PhysicsError> create(const CharacterDesc& desc);

    // Advances the character by frame_seconds against the world's current colliders. `horizontal_velocity` is the walk velocity in
    // m/s; its y component is ignored. `jump` starts a jump when the character is on the ground. Fails with InvalidState for a closed world.
    [[nodiscard]] core::Status update(const PhysicsWorld& world, const glm::vec3& horizontal_velocity, bool jump, double frame_seconds);

    // Places the character at `position` with no velocity and no ground.
    void teleport(const glm::dvec3& position);

    [[nodiscard]] glm::dvec3 position() const noexcept;  // the bottom of the capsule
    [[nodiscard]] glm::vec3 velocity() const noexcept;   // the last walk velocity, with the vertical velocity
    [[nodiscard]] bool grounded() const noexcept;
    [[nodiscard]] ecs::EntityHandle ground() const noexcept;  // the body the character stands on, when it stands on one

    // Entities of the bodies the capsule overlaps now, triggers included.
    [[nodiscard]] std::vector<ecs::EntityHandle> overlaps(const PhysicsWorld& world) const;

private:
    // One pass of a move: how far the feet got, and what stopped them.
    struct Slide {
        glm::dvec3 feet;
        bool blocked = false;
        bool blocked_by_walkable = false;
    };

    explicit CharacterController(const CharacterDesc& desc);

    [[nodiscard]] std::optional<QueryHit> sweep(const PhysicsWorld& world, const glm::dvec3& feet, const glm::vec3& direction,
                                                float distance) const;
    [[nodiscard]] bool walkable(const glm::vec3& normal) const noexcept;
    [[nodiscard]] Slide slide(const PhysicsWorld& world, const glm::dvec3& feet, const glm::vec3& motion, bool allow_step) const;
    [[nodiscard]] std::optional<glm::dvec3> try_step(const PhysicsWorld& world, const glm::dvec3& feet, const glm::vec3& horizontal) const;
    void carry(const PhysicsWorld& world);
    void settle(const PhysicsWorld& world, float reach);

    float radius_;
    float half_height_;
    float max_slope_cos_;
    float step_height_;
    float jump_speed_;
    glm::vec3 gravity_;
    QueryFilter filter_;
    glm::dvec3 feet_;
    glm::vec3 velocity_{0.0f};
    bool grounded_ = false;
    ecs::EntityHandle ground_;     // valid while grounded on a body
    PhysicsTransform ground_pose_;  // the ground body's pose at the last update; the platform's motion is measured from it
};

}  // namespace engine::physics
