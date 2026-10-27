#pragma once

// Physics adapter (US-052). A PhysicsWorld owns one Jolt physics system for one runtime scene. Simulation advances in fixed steps of
// 1/60 s, and a body's render transform interpolates between its last two steps. Bodies refer to ecs entities by handle, and contact
// and activation callbacks are queued from Jolt's worker threads and taken on the owning thread between updates. No Jolt type appears
// here.
//
// Jolt keeps single-precision positions relative to the large-world origin (origin.hpp). The world is an OriginSubscriber through
// origin_subscriber(), and it moves those positions by each shift. The public API takes and returns world positions in double, and the
// interpolation history is kept in double, so a rebase never changes a returned transform.

#include "engine/core/result.hpp"
#include "engine/core/status.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/world/origin.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace engine::physics {

inline constexpr double kFixedRateHz = 60.0;
inline constexpr double kFixedStep = 1.0 / kFixedRateHz;  // seconds per simulation step
inline constexpr std::uint32_t kDefaultMaxCatchUpSteps = 4;

enum class BodyMotion { Static, Dynamic, Kinematic };
enum class ShapeKind { Box, Sphere, Capsule, ConvexHull, Mesh, Heightfield };

// A square grid of height samples for a Heightfield collider. Samples are row-major, `size * size` values in meters above the body's
// position. Sample (0, 0) sits at the negative corner, so the grid is centred on the body in x and z.
inline constexpr std::uint32_t kMaxHeightfieldSize = 1024;
inline constexpr std::uint32_t kMaxCollisionLayers = 32;

struct BodyDesc {
    ecs::EntityHandle entity;
    BodyMotion motion = BodyMotion::Dynamic;
    ShapeKind shape = ShapeKind::Box;
    glm::vec3 half_extents{0.5f, 0.5f, 0.5f};  // Box only; every component must be positive
    float radius = 0.5f;                       // Sphere and Capsule; must be positive
    float half_height = 0.5f;                  // Capsule only: half the length of the cylinder between the end caps; not negative
    std::vector<glm::vec3> points;             // ConvexHull only: at least four points that are not coplanar
    std::vector<glm::vec3> vertices;           // Mesh only: triangle vertices
    std::vector<std::uint32_t> indices;        // Mesh only: triangles as index triplets into vertices
    std::uint32_t grid_size = 0;               // Heightfield only: samples per side, at least two
    std::vector<float> heights;                // Heightfield only: grid_size * grid_size samples
    float grid_spacing = 1.0f;                 // Heightfield only: meters between neighbouring samples; must be positive
    glm::dvec3 position{0.0};                  // world meters
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float friction = 0.5f;
    float restitution = 0.0f;
    // Collision filter: this body and another collide only when each one's mask includes the other one's layer. Layers are 0..31.
    std::uint32_t layer = 0;
    std::uint32_t mask = 0xFFFFFFFFu;
    // A trigger (sensor) reports contacts and is found by queries, but it never pushes other bodies.
    bool trigger = false;
};

// Which bodies a query may return. A body is tested when its layer bit is set in `layers`.
struct QueryFilter {
    std::uint32_t layers = 0xFFFFFFFFu;
    bool include_triggers = true;
    ecs::EntityHandle ignore;  // never returned; invalid means none
};

// A query result. `position` and `normal` are world space; the entity is the one the hit body refers to. A raycast's normal is the
// surface normal at the hit. A shape cast's normal is the contact normal, which points from the hit body toward the cast shape.
struct QueryHit {
    ecs::EntityHandle entity;
    glm::dvec3 position{0.0};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    double distance = 0.0;  // meters from the start of the cast to the hit
    bool trigger = false;
};

enum class ConstraintKind { Fixed, Hinge, Distance };

// A joint between two bodies (US-054). Every anchor and axis is body-local, in meters from that body's centre of mass, which is the body
// origin for box, sphere, and capsule colliders. A fixed joint welds the two anchor points together and holds the two frames still
// relative to each other. A hinge keeps the anchors together and lets the bodies rotate about their hinge axes; its rotation is limited
// to [min_limit, max_limit] radians, with the default unlimited. A distance joint keeps the anchors between min_distance and max_distance
// meters apart. Each frame is an axis pair: `axis` (hinge axis, or the frame's Y) and `normal` (the frame's X) must be perpendicular and
// non-zero, for each body.
struct ConstraintDesc {
    ConstraintKind kind = ConstraintKind::Fixed;
    ecs::EntityHandle body_a;  // two different bodies in the world
    ecs::EntityHandle body_b;
    glm::vec3 anchor_a{0.0f};
    glm::vec3 anchor_b{0.0f};
    glm::vec3 axis_a{0.0f, 1.0f, 0.0f};
    glm::vec3 axis_b{0.0f, 1.0f, 0.0f};
    glm::vec3 normal_a{1.0f, 0.0f, 0.0f};
    glm::vec3 normal_b{1.0f, 0.0f, 0.0f};
    float min_limit = -3.14159265358979f;  // hinge only: radians, in [-pi, pi], min <= max
    float max_limit = 3.14159265358979f;
    float min_distance = 0.0f;  // distance only: meters, 0 <= min <= max, max > 0
    float max_distance = 1.0f;
};

// Names one constraint in one world. Handles are never reused, so a stale handle matches no constraint.
struct ConstraintHandle {
    std::uint64_t value = 0;  // zero is never a live constraint

    [[nodiscard]] bool valid() const noexcept {
        return value != 0;
    }
    bool operator==(const ConstraintHandle& other) const noexcept {
        return value == other.value;
    }
};

struct PhysicsWorldDesc {
    glm::dvec3 gravity{0.0, -9.81, 0.0};  // m/s^2
    glm::dvec3 origin{0.0};               // large-world origin the single-precision positions are relative to
    std::uint32_t max_bodies = 4096;
    std::uint32_t worker_threads = 1;     // 0 runs every job on the calling thread
    std::uint32_t max_catch_up_steps = kDefaultMaxCatchUpSteps;  // fixed steps one update may run; the rest of its time is dropped
};

enum class PhysicsError {
    InvalidState,   // the world is closed
    InvalidEntity,  // the handle is invalid, or the entity already has a body
    InvalidDesc,    // a size, count, or vector is not usable
    BodyLimit,      // the world has no free body slot
};

enum class PhysicsEventKind {
    ContactAdded,
    ContactRemoved,
    BodyActivated,
    BodyDeactivated,
};

// One callback, published through the queue. For contacts, `first` and `second` are the two bodies. For activation events, only
// `first` is set. Each event is tagged with the fixed step during which it was raised.
struct PhysicsEvent {
    PhysicsEventKind kind = PhysicsEventKind::ContactAdded;
    ecs::EntityHandle first;
    ecs::EntityHandle second;
    std::uint64_t step = 0;
};

struct PhysicsTransform {
    glm::dvec3 position{0.0};  // world meters
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct PhysicsStats {
    std::uint64_t steps = 0;           // fixed steps run since the world was created
    std::uint64_t catch_up_events = 0; // updates that reached the catch-up limit with time left over
    std::uint64_t dropped_steps = 0;   // fixed steps discarded by the catch-up limit
    std::uint64_t update_errors = 0;   // steps Jolt reported as failed
};

namespace detail {
struct PhysicsState;
}

class PhysicsWorld {
public:
    // Creates a world. Jolt is initialised on first use; every world shares that process-wide setup.
    [[nodiscard]] static core::Result<std::unique_ptr<PhysicsWorld>, PhysicsError> create(const PhysicsWorldDesc& desc);

    // Closes the world: every body is removed and destroyed, and queued events are dropped.
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    // Adds a body that refers to `desc.entity`. The entity may own at most one body in this world.
    [[nodiscard]] core::Result<ecs::EntityHandle, PhysicsError> add_body(const BodyDesc& desc);

    // Removes and destroys the body of `entity`. Returns false when the entity has no body here. A stale handle never matches a
    // body that a later entity received in the same slot. Every constraint that names the body is removed first, so no joint is
    // left pointing at a destroyed body.
    bool remove_body(ecs::EntityHandle entity);

    // Adds a joint between two bodies that are in this world. Fails with InvalidEntity for a missing body, with InvalidDesc for
    // a body named twice, a non-finite or non-perpendicular axis pair, or limits out of range, and with InvalidState for a closed world.
    [[nodiscard]] core::Result<ConstraintHandle, PhysicsError> add_constraint(const ConstraintDesc& desc);

    // Removes one constraint. Returns false for a stale handle, or for a closed world.
    bool remove_constraint(ConstraintHandle handle);

    [[nodiscard]] std::size_t constraint_count() const;

    [[nodiscard]] bool has_body(ecs::EntityHandle entity) const;
    [[nodiscard]] std::size_t body_count() const;

    // Sets the target of a kinematic body. The body reaches it during the next fixed step. Returns false for a missing body, a body
    // that is not kinematic, or a closed world.
    bool move_kinematic(ecs::EntityHandle entity, const glm::dvec3& position, const glm::quat& rotation);

    // Sets the linear velocity of a dynamic body and wakes it, so a sleeping body moves again (the physics benchmark uses this as its
    // interaction). Returns false for a missing or non-dynamic body, a non-finite velocity, or a closed world.
    bool set_linear_velocity(ecs::EntityHandle entity, const glm::vec3& velocity);

    // Rigid bodies that are awake (dynamic and kinematic). Static bodies and sleeping bodies are not counted, so the dynamic bodies at
    // rest are the dynamic count minus this. Zero for a closed world.
    [[nodiscard]] std::size_t active_body_count() const;

    // Replaces the collider of the body that `entity` owns. The body keeps its id, motion, pose, and velocity; its shape, filter, and
    // trigger flag come from `desc`, whose entity must match. Fails with InvalidEntity for a missing body or a mismatched entity, with
    // InvalidDesc for an invalid description or a changed motion type, and with InvalidState for a closed world. The old shape is released.
    [[nodiscard]] core::Result<ecs::EntityHandle, PhysicsError> replace_collider(ecs::EntityHandle entity, const BodyDesc& desc);

    // Nearest body along a ray from `origin` (world meters) for up to `max_distance` meters. `direction` need not be normalised.
    [[nodiscard]] std::optional<QueryHit> raycast(const glm::dvec3& origin, const glm::vec3& direction, float max_distance,
                                                  const QueryFilter& filter = {}) const;

    // Nearest body that the shape reaches when moved from its pose in `shape` along `direction` for up to `max_distance` meters.
    // Only the shape, position, and rotation of `shape` are used; its motion, filter, and trigger flag are ignored.
    [[nodiscard]] std::optional<QueryHit> shape_cast(const BodyDesc& shape, const glm::vec3& direction, float max_distance,
                                                     const QueryFilter& filter = {}) const;

    // True when a dynamic or kinematic body's current position lies in the box [min, max] (world meters).
    [[nodiscard]] bool has_moving_body_in(const glm::dvec3& min, const glm::dvec3& max) const;

    // Entities of the bodies whose shape `shape` overlaps now (world meters). Bodies that only touch, or lie beyond the shape, are not
    // returned. Triggers are returned when the filter includes them. The shape's motion, trigger flag, and entity are ignored.
    [[nodiscard]] std::vector<ecs::EntityHandle> overlaps(const BodyDesc& shape, const QueryFilter& filter = {}) const;

    // Advances the simulation by the frame time. While paused, the time is discarded and no step runs. Otherwise the time is accumulated
    // and fixed steps run, up to max_catch_up_steps; time beyond that is dropped and counted. Returns InvalidState when the world is closed.
    [[nodiscard]] core::Status update(double frame_seconds);

    void set_paused(bool paused) noexcept;
    [[nodiscard]] bool paused() const noexcept;

    // Fraction of a fixed step, in [0, 1), between the last step and the next one.
    [[nodiscard]] double interpolation_alpha() const noexcept;

    // Transform as of the last fixed step.
    [[nodiscard]] std::optional<PhysicsTransform> step_transform(ecs::EntityHandle entity) const;
    // Transform interpolated at interpolation_alpha() between the last two steps.
    [[nodiscard]] std::optional<PhysicsTransform> render_transform(ecs::EntityHandle entity) const;

    // Returns and clears every event raised since the last call, in the order the queue received them.
    [[nodiscard]] std::vector<PhysicsEvent> take_events();

    [[nodiscard]] PhysicsStats stats() const;
    [[nodiscard]] glm::dvec3 origin() const;

    // Subscribe to the large-world origin with this. The subscriber stays valid after the world closes or is destroyed, and then it does nothing.
    [[nodiscard]] std::shared_ptr<world::OriginSubscriber> origin_subscriber();

    // Closes the world. Later calls to update() and add_body() return InvalidState, queries return nothing, and take_events() is empty.
    void close();
    [[nodiscard]] bool closed() const noexcept;

private:
    PhysicsWorld();

    std::unique_ptr<detail::PhysicsState> state_;
};

}  // namespace engine::physics
