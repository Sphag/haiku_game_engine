#include "engine/physics/physics_world.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/log.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::physics {

namespace detail {
namespace {

// Two broad-phase layers: static bodies, and everything that moves. Each body owns one object layer, an index into a table that
// holds its filter bit, its mask, and whether it moves. The table changes only between steps.
constexpr JPH::BroadPhaseLayer kStaticBroadPhase{0};
constexpr JPH::BroadPhaseLayer kMovingBroadPhase{1};
constexpr JPH::uint kBroadPhaseLayerCount = 2;

constexpr JPH::uint kMaxJobs = 2048;
constexpr JPH::uint kMaxBarriers = 8;
constexpr JPH::uint kMaxBodyPairs = 65536;
constexpr JPH::uint kMaxContactConstraints = 10240;
constexpr std::size_t kTempAllocatorBytes = 16 * 1024 * 1024;

// Absorbs rounding in the frame-time accumulator, so a frame of exactly one step runs that step.
constexpr double kStepEpsilon = 1e-9;

struct CollisionFilter {
    std::uint32_t bit = 0;   // 1 << layer
    std::uint32_t mask = 0;  // layers this body collides with
    bool moving = false;     // dynamic or kinematic
};

struct CollisionTable {
    std::vector<CollisionFilter> entries;  // indexed by object layer
    std::vector<JPH::ObjectLayer> free_ids;
};

CollisionFilter make_filter(const BodyDesc& desc) {
    return CollisionFilter{std::uint32_t{1} << desc.layer, desc.mask, desc.motion != BodyMotion::Static};
}

JPH::ObjectLayer allocate_object_layer(CollisionTable& table) {
    if (!table.free_ids.empty()) {
        const JPH::ObjectLayer id = table.free_ids.back();
        table.free_ids.pop_back();
        return id;
    }
    table.entries.emplace_back();
    return static_cast<JPH::ObjectLayer>(table.entries.size() - 1);
}

void free_object_layer(CollisionTable& table, JPH::ObjectLayer id) {
    table.entries[id] = CollisionFilter{};
    table.free_ids.push_back(id);
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    explicit BroadPhaseLayers(const CollisionTable& table) : table_(table) {}

    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
        return kBroadPhaseLayerCount;
    }

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return table_.entries[layer].moving ? kMovingBroadPhase : kStaticBroadPhase;
    }

private:
    const CollisionTable& table_;
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    explicit ObjectVsBroadPhase(const CollisionTable& table) : table_(table) {}

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase) const override {
        return table_.entries[layer].moving || broad_phase == kMovingBroadPhase;
    }

private:
    const CollisionTable& table_;
};

// Static bodies never collide with each other. Two bodies collide when each one's mask includes the other one's layer.
class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectPairs(const CollisionTable& table) : table_(table) {}

    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        const CollisionFilter& first = table_.entries[a];
        const CollisionFilter& second = table_.entries[b];
        if (!first.moving && !second.moving) {
            return false;
        }
        return (first.mask & second.bit) != 0 && (second.mask & first.bit) != 0;
    }

private:
    const CollisionTable& table_;
};

// Queries test a body's layer against the caller's mask and skip triggers when asked. The ignored body is never returned.
class QueryBodies final : public JPH::BodyFilter {
public:
    QueryBodies(const CollisionTable& table, const QueryFilter& filter, JPH::BodyID ignored)
        : table_(table), filter_(filter), ignored_(ignored) {}

    [[nodiscard]] bool ShouldCollide(const JPH::BodyID& id) const override {
        return id != ignored_;
    }

    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        if ((filter_.layers & table_.entries[body.GetObjectLayer()].bit) == 0) {
            return false;
        }
        return filter_.include_triggers || !body.IsSensor();
    }

private:
    const CollisionTable& table_;
    QueryFilter filter_;
    JPH::BodyID ignored_;
};

// Jolt calls these on its worker threads. The owning thread changes the entity map only between steps, so the callbacks read it
// without a lock and push onto the event queue under its mutex.
class PhysicsCallbacks final : public JPH::ContactListener, public JPH::BodyActivationListener {
public:
    explicit PhysicsCallbacks(const std::unordered_map<std::uint32_t, ecs::EntityHandle>& entity_by_body) : entity_by_body_(entity_by_body) {}

    void set_step(std::uint64_t step) noexcept {
        step_.store(step, std::memory_order_relaxed);
    }

    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold&, JPH::ContactSettings&) override {
        publish(PhysicsEventKind::ContactAdded, body1.GetID(), body2.GetID());
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        publish(PhysicsEventKind::ContactRemoved, pair.GetBody1ID(), pair.GetBody2ID());
    }

    void OnBodyActivated(const JPH::BodyID& id, JPH::uint64) override {
        publish(PhysicsEventKind::BodyActivated, id, std::nullopt);
    }

    void OnBodyDeactivated(const JPH::BodyID& id, JPH::uint64) override {
        publish(PhysicsEventKind::BodyDeactivated, id, std::nullopt);
    }

    [[nodiscard]] std::vector<PhysicsEvent> take() {
        std::lock_guard lock(mutex_);
        return std::exchange(events_, {});
    }

    void discard() {
        std::lock_guard lock(mutex_);
        events_.clear();
    }

private:
    [[nodiscard]] std::optional<ecs::EntityHandle> find(JPH::BodyID id) const {
        const auto it = entity_by_body_.find(id.GetIndexAndSequenceNumber());
        if (it == entity_by_body_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // An event for a body the world does not know (for example, one removed during this step) is dropped.
    void publish(PhysicsEventKind kind, JPH::BodyID first, std::optional<JPH::BodyID> second) {
        const std::optional<ecs::EntityHandle> a = find(first);
        if (!a) {
            return;
        }
        ecs::EntityHandle b;
        if (second) {
            const std::optional<ecs::EntityHandle> found = find(*second);
            if (!found) {
                return;
            }
            b = *found;
        }
        const PhysicsEvent event{kind, *a, b, step_.load(std::memory_order_relaxed)};
        std::lock_guard lock(mutex_);
        events_.push_back(event);
    }

    const std::unordered_map<std::uint32_t, ecs::EntityHandle>& entity_by_body_;
    std::atomic<std::uint64_t> step_{0};
    std::mutex mutex_;
    std::vector<PhysicsEvent> events_;
};

struct EntityHash {
    [[nodiscard]] std::size_t operator()(const ecs::EntityHandle& handle) const noexcept {
        const std::size_t slot = std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(handle.index) << 32) | handle.generation);
        return slot ^ (std::hash<std::uint32_t>{}(handle.world) + 0x9e3779b97f4a7c15ULL + (slot << 6) + (slot >> 2));
    }
};

struct BodyRecord {
    JPH::BodyID id;
    BodyMotion motion = BodyMotion::Dynamic;
    PhysicsTransform previous;  // as of the step before the last one, in world space
    PhysicsTransform current;   // as of the last step, in world space
    JPH::ObjectLayer object_layer = 0;
};

struct ConstraintRecord {
    ecs::EntityHandle body_a;
    ecs::EntityHandle body_b;
    JPH::Ref<JPH::TwoBodyConstraint> joint;  // owned here; the system holds a raw pointer
};

// Forwards each origin shift to its world. The world detaches it when it closes, so a subscriber that outlives the world does nothing.
class OriginLink final : public world::OriginSubscriber {
public:
    void on_origin_shifted(const world::OriginShift& shift) override;

    void attach(PhysicsState* state) noexcept {
        state_ = state;
    }

    void detach() noexcept {
        state_ = nullptr;
    }

private:
    PhysicsState* state_ = nullptr;
};

}  // namespace

struct PhysicsState {
    explicit PhysicsState(const PhysicsWorldDesc& desc);

    glm::dvec3 origin;
    std::uint32_t max_catch_up_steps;
    std::unordered_map<std::uint32_t, ecs::EntityHandle> entity_by_body;  // keyed by BodyID; written only between steps
    std::unordered_map<ecs::EntityHandle, BodyRecord, EntityHash> bodies;
    std::unordered_map<std::uint64_t, ConstraintRecord> constraints;  // keyed by ConstraintHandle::value
    std::uint64_t next_constraint = 1;                                // handles are never reused
    std::unique_ptr<JPH::JobSystem> jobs;
    std::unique_ptr<JPH::TempAllocator> temp;
    CollisionTable table;
    BroadPhaseLayers broad_phase_layers{table};
    ObjectVsBroadPhase object_vs_broad_phase{table};
    ObjectPairs object_pairs{table};
    std::unique_ptr<PhysicsCallbacks> callbacks;
    std::unique_ptr<JPH::PhysicsSystem> system;  // declared last, so it is destroyed before the filters and callbacks it points to
    std::shared_ptr<OriginLink> link;
    double accumulator = 0.0;  // seconds not yet simulated
    double alpha = 0.0;
    bool paused = false;
    bool closed = false;
    PhysicsStats stats;
};

PhysicsState::PhysicsState(const PhysicsWorldDesc& desc)
    : origin(desc.origin), max_catch_up_steps(desc.max_catch_up_steps) {
    if (desc.worker_threads == 0) {
        jobs = std::make_unique<JPH::JobSystemSingleThreaded>(kMaxJobs);
    } else {
        jobs = std::make_unique<JPH::JobSystemThreadPool>(kMaxJobs, kMaxBarriers, static_cast<int>(desc.worker_threads));
    }
    temp = std::make_unique<JPH::TempAllocatorImpl>(kTempAllocatorBytes);
    callbacks = std::make_unique<PhysicsCallbacks>(entity_by_body);
    system = std::make_unique<JPH::PhysicsSystem>();
    system->Init(desc.max_bodies, 0, kMaxBodyPairs, kMaxContactConstraints, broad_phase_layers, object_vs_broad_phase, object_pairs);
    system->SetGravity(JPH::Vec3(static_cast<float>(desc.gravity.x), static_cast<float>(desc.gravity.y), static_cast<float>(desc.gravity.z)));
    system->SetContactListener(callbacks.get());
    system->SetBodyActivationListener(callbacks.get());
    link = std::make_shared<OriginLink>();
    link->attach(this);
}

namespace {

// Process-wide Jolt setup, run once on the first world. Trace and assert output goes to the engine log.
void trace_to_log(const char* format, ...) {
    char buffer[1024];
    std::va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    core::log_message(core::LogLevel::Debug, "physics", buffer);
}

bool assert_to_log(const char* expression, const char* message, const char* file, JPH::uint line) {
    const std::string text = std::string("assertion failed: ") + expression + " (" + (message != nullptr ? message : "") + ") at " + file +
                             ":" + std::to_string(line);
    core::log_message(core::LogLevel::Error, "physics", text);
    return true;  // break into the debugger, as Jolt does by default
}

void initialise_jolt() {
    static const bool initialised = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace_to_log;
        JPH::AssertFailed = assert_to_log;
        // The factory is process-lifetime: Jolt's type registry points at it, so it is never deleted.
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    static_cast<void>(initialised);
}

bool finite(const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const glm::quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

bool finite3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool valid_shape(const BodyDesc& desc) {
    switch (desc.shape) {
        case ShapeKind::Box:
            return finite3(desc.half_extents) && desc.half_extents.x > 0.0f && desc.half_extents.y > 0.0f && desc.half_extents.z > 0.0f;
        case ShapeKind::Sphere:
            return std::isfinite(desc.radius) && desc.radius > 0.0f;
        case ShapeKind::Capsule:
            return std::isfinite(desc.radius) && desc.radius > 0.0f && std::isfinite(desc.half_height) && desc.half_height > 0.0f;
        case ShapeKind::ConvexHull:
            if (desc.points.size() < 4) {
                return false;
            }
            return std::all_of(desc.points.begin(), desc.points.end(), finite3);
        case ShapeKind::Mesh:
            if (desc.motion == BodyMotion::Dynamic || desc.vertices.empty() || desc.indices.empty() || desc.indices.size() % 3 != 0 ||
                !std::all_of(desc.vertices.begin(), desc.vertices.end(), finite3)) {
                return false;
            }
            return std::all_of(desc.indices.begin(), desc.indices.end(),
                               [&](std::uint32_t index) { return index < desc.vertices.size(); });
        case ShapeKind::Heightfield:
            if (desc.motion == BodyMotion::Dynamic || desc.grid_size < 2 || desc.grid_size > kMaxHeightfieldSize ||
                !std::isfinite(desc.grid_spacing) || desc.grid_spacing <= 0.0f) {
                return false;
            }
            return desc.heights.size() == static_cast<std::size_t>(desc.grid_size) * desc.grid_size &&
                   std::all_of(desc.heights.begin(), desc.heights.end(), [](float h) { return std::isfinite(h); });
    }
    return false;
}

bool valid_desc(const BodyDesc& desc) {
    if (!finite(desc.position) || !finite(desc.rotation) || glm::length(desc.rotation) <= 0.0f) {
        return false;
    }
    if (!std::isfinite(desc.friction) || desc.friction < 0.0f || !std::isfinite(desc.restitution) || desc.restitution < 0.0f ||
        desc.restitution > 1.0f || desc.layer >= kMaxCollisionLayers) {
        return false;
    }
    return valid_shape(desc);
}

JPH::ShapeSettings::ShapeResult make_shape(const BodyDesc& desc) {
    switch (desc.shape) {
        case ShapeKind::Box:
            return JPH::BoxShapeSettings(JPH::Vec3(desc.half_extents.x, desc.half_extents.y, desc.half_extents.z)).Create();
        case ShapeKind::Sphere:
            return JPH::SphereShapeSettings(desc.radius).Create();
        case ShapeKind::Capsule:
            return JPH::CapsuleShapeSettings(desc.half_height, desc.radius).Create();
        case ShapeKind::ConvexHull: {
            JPH::Array<JPH::Vec3> points;
            points.reserve(desc.points.size());
            for (const glm::vec3& p : desc.points) {
                points.push_back(JPH::Vec3(p.x, p.y, p.z));
            }
            return JPH::ConvexHullShapeSettings(points).Create();
        }
        case ShapeKind::Mesh: {
            JPH::VertexList vertices;
            vertices.reserve(desc.vertices.size());
            for (const glm::vec3& v : desc.vertices) {
                vertices.push_back(JPH::Float3(v.x, v.y, v.z));
            }
            JPH::IndexedTriangleList triangles;
            triangles.reserve(desc.indices.size() / 3);
            for (std::size_t i = 0; i < desc.indices.size(); i += 3) {
                triangles.push_back(JPH::IndexedTriangle(desc.indices[i], desc.indices[i + 1], desc.indices[i + 2], 0));
            }
            return JPH::MeshShapeSettings(vertices, triangles).Create();
        }
        case ShapeKind::Heightfield: {
            // The grid is centred on the body in x and z, and each sample's height is in meters.
            const float spacing = desc.grid_spacing;
            const float half_span = 0.5f * static_cast<float>(desc.grid_size - 1) * spacing;
            return JPH::HeightFieldShapeSettings(desc.heights.data(), JPH::Vec3(-half_span, 0.0f, -half_span),
                                                 JPH::Vec3(spacing, 1.0f, spacing), desc.grid_size)
                .Create();
        }
    }
    return JPH::BoxShapeSettings(JPH::Vec3(0.5f, 0.5f, 0.5f)).Create();
}

JPH::EMotionType motion_type(BodyMotion motion) {
    switch (motion) {
        case BodyMotion::Static:
            return JPH::EMotionType::Static;
        case BodyMotion::Dynamic:
            return JPH::EMotionType::Dynamic;
        case BodyMotion::Kinematic:
            return JPH::EMotionType::Kinematic;
    }
    return JPH::EMotionType::Static;
}

JPH::RVec3 to_jolt(const glm::vec3& v) {
    return JPH::RVec3(v.x, v.y, v.z);
}

JPH::Quat to_jolt(const glm::quat& q) {
    return JPH::Quat(q.x, q.y, q.z, q.w);
}

// Jolt's single-precision position, relative to the origin, in world meters.
PhysicsTransform read_body(const JPH::BodyInterface& bodies, JPH::BodyID id, const glm::dvec3& origin) {
    JPH::RVec3 position;
    JPH::Quat rotation;
    bodies.GetPositionAndRotation(id, position, rotation);
    return PhysicsTransform{origin + glm::dvec3(position.GetX(), position.GetY(), position.GetZ()),
                            glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ())};
}

glm::vec3 to_local(const glm::dvec3& world, const glm::dvec3& origin) {
    return glm::vec3(world - origin);
}

// An axis and a reference axis must be non-zero and perpendicular, so the frame they form is orthogonal.
bool orthogonal_frame(const glm::vec3& axis, const glm::vec3& normal) {
    if (!finite3(axis) || !finite3(normal) || glm::length(axis) <= 0.0f || glm::length(normal) <= 0.0f) {
        return false;
    }
    return std::abs(glm::dot(glm::normalize(axis), glm::normalize(normal))) < 1e-3f;
}

bool valid_constraint(const ConstraintDesc& desc) {
    if (desc.body_a == desc.body_b || !finite3(desc.anchor_a) || !finite3(desc.anchor_b) || !orthogonal_frame(desc.axis_a, desc.normal_a) ||
        !orthogonal_frame(desc.axis_b, desc.normal_b)) {
        return false;
    }
    switch (desc.kind) {
        case ConstraintKind::Fixed:
            return true;
        case ConstraintKind::Hinge:
            return std::isfinite(desc.min_limit) && std::isfinite(desc.max_limit) && -JPH::JPH_PI <= desc.min_limit &&
                   desc.min_limit <= desc.max_limit && desc.max_limit <= JPH::JPH_PI;
        case ConstraintKind::Distance:
            return std::isfinite(desc.min_distance) && std::isfinite(desc.max_distance) && desc.min_distance >= 0.0f &&
                   desc.min_distance <= desc.max_distance && desc.max_distance > 0.0f;
    }
    return false;
}

// The anchors and axes are body-local (LocalToBodyCOM), so the joint keeps its frame when either body moves.
JPH::Ref<JPH::TwoBodyConstraint> make_constraint(const ConstraintDesc& desc, JPH::Body& first, JPH::Body& second) {
    const glm::vec3 axis_a = glm::normalize(desc.axis_a);
    const glm::vec3 axis_b = glm::normalize(desc.axis_b);
    const glm::vec3 normal_a = glm::normalize(desc.normal_a);
    const glm::vec3 normal_b = glm::normalize(desc.normal_b);
    switch (desc.kind) {
        case ConstraintKind::Fixed: {
            JPH::FixedConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mPoint1 = to_jolt(desc.anchor_a);
            settings.mAxisX1 = to_jolt(normal_a);
            settings.mAxisY1 = to_jolt(axis_a);
            settings.mPoint2 = to_jolt(desc.anchor_b);
            settings.mAxisX2 = to_jolt(normal_b);
            settings.mAxisY2 = to_jolt(axis_b);
            return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(first, second));
        }
        case ConstraintKind::Hinge: {
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mPoint1 = to_jolt(desc.anchor_a);
            settings.mHingeAxis1 = to_jolt(axis_a);
            settings.mNormalAxis1 = to_jolt(normal_a);
            settings.mPoint2 = to_jolt(desc.anchor_b);
            settings.mHingeAxis2 = to_jolt(axis_b);
            settings.mNormalAxis2 = to_jolt(normal_b);
            settings.mLimitsMin = desc.min_limit;
            settings.mLimitsMax = desc.max_limit;
            return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(first, second));
        }
        case ConstraintKind::Distance: {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
            settings.mPoint1 = to_jolt(desc.anchor_a);
            settings.mPoint2 = to_jolt(desc.anchor_b);
            settings.mMinDistance = desc.min_distance;
            settings.mMaxDistance = desc.max_distance;
            return JPH::Ref<JPH::TwoBodyConstraint>(settings.Create(first, second));
        }
    }
    return nullptr;
}

void run_step(PhysicsState& state) {
    for (auto& [entity, record] : state.bodies) {
        record.previous = record.current;
    }
    ++state.stats.steps;
    state.callbacks->set_step(state.stats.steps);

    const JPH::EPhysicsUpdateError error =
        state.system->Update(static_cast<float>(kFixedStep), 1, state.temp.get(), state.jobs.get());
    if (error != JPH::EPhysicsUpdateError::None) {
        ++state.stats.update_errors;
    }

    const JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    for (auto& [entity, record] : state.bodies) {
        if (record.motion != BodyMotion::Static) {
            record.current = read_body(bodies, record.id, state.origin);
        }
    }
}

// Moves every body's single-precision position by the shift. Static bodies are included, because their positions are relative to the
// origin too. The world-space history is unchanged, so no returned transform moves.
void apply_origin_shift(PhysicsState& state, const world::OriginShift& shift) {
    JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    for (const auto& [entity, record] : state.bodies) {
        const JPH::RVec3 position = bodies.GetPosition(record.id);
        const glm::vec3 rebased = shift.rebase_local(glm::vec3(position.GetX(), position.GetY(), position.GetZ()));
        bodies.SetPosition(record.id, to_jolt(rebased), JPH::EActivation::DontActivate);
    }
    state.origin = shift.to;
}

void OriginLink::on_origin_shifted(const world::OriginShift& shift) {
    if (state_ != nullptr && !state_->closed) {
        apply_origin_shift(*state_, shift);
    }
}

core::Status update_state(PhysicsState& state, double frame_seconds) {
    if (state.closed) {
        return {core::StatusCode::InvalidState};
    }
    if (state.paused) {
        return {};
    }

    state.accumulator += frame_seconds;
    std::uint32_t steps_this_update = 0;
    while (state.accumulator + kStepEpsilon >= kFixedStep && steps_this_update < state.max_catch_up_steps) {
        run_step(state);
        state.accumulator -= kFixedStep;
        ++steps_this_update;
    }
    if (state.accumulator + kStepEpsilon >= kFixedStep) {
        // The limit stopped the loop with at least one step of time left. Drop whole steps and keep the fraction for interpolation.
        const double whole = std::floor((state.accumulator + kStepEpsilon) / kFixedStep);
        state.stats.dropped_steps += static_cast<std::uint64_t>(whole);
        ++state.stats.catch_up_events;
        state.accumulator -= whole * kFixedStep;
    }
    state.accumulator = std::max(state.accumulator, 0.0);
    state.alpha = std::clamp(state.accumulator / kFixedStep, 0.0, 1.0);
    return {};
}

void close_state(PhysicsState& state) {
    if (state.closed) {
        return;
    }
    state.closed = true;
    state.link->detach();

    // Joints go first: they refer to the bodies that are removed below.
    for (const auto& [handle, record] : state.constraints) {
        state.system->RemoveConstraint(record.joint.GetPtr());
    }
    state.constraints.clear();

    // Removing a body may raise contact callbacks, so the entity map is still complete here. The queue is cleared afterwards.
    JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    for (const auto& [entity, record] : state.bodies) {
        bodies.RemoveBody(record.id);
        bodies.DestroyBody(record.id);
    }
    state.bodies.clear();
    state.entity_by_body.clear();
    state.table = CollisionTable{};
    state.callbacks->discard();

    state.system.reset();
    state.callbacks.reset();
    state.temp.reset();
    state.jobs.reset();  // joins the worker threads
}

// A body that is asleep stays asleep when a joint changes, so a joint added or removed wakes its dynamic bodies. Static bodies never move.
void wake(PhysicsState& state, ecs::EntityHandle entity) {
    const auto it = state.bodies.find(entity);
    if (it != state.bodies.end() && it->second.motion != BodyMotion::Static) {
        state.system->GetBodyInterface().ActivateBody(it->second.id);
    }
}

// Removes every joint that names `entity`, before its body goes, so the system never holds a joint to a destroyed body.
void remove_constraints_of(PhysicsState& state, ecs::EntityHandle entity) {
    for (auto it = state.constraints.begin(); it != state.constraints.end();) {
        if (it->second.body_a == entity || it->second.body_b == entity) {
            state.system->RemoveConstraint(it->second.joint.GetPtr());
            wake(state, it->second.body_a);
            wake(state, it->second.body_b);
            it = state.constraints.erase(it);
        } else {
            ++it;
        }
    }
}

// Builds a query result for a hit on `id`. The point is Jolt-local; the normal comes from the body at that point.
QueryHit describe(const PhysicsState& state, JPH::BodyID id, JPH::SubShapeID sub_shape, JPH::RVec3 point, double distance) {
    QueryHit hit;
    const auto entity = state.entity_by_body.find(id.GetIndexAndSequenceNumber());
    if (entity != state.entity_by_body.end()) {
        hit.entity = entity->second;
    }
    hit.position = state.origin + glm::dvec3(point.GetX(), point.GetY(), point.GetZ());
    hit.distance = distance;
    JPH::BodyLockRead lock(state.system->GetBodyLockInterface(), id);
    if (lock.Succeeded()) {
        const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(sub_shape, point);
        hit.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
        hit.trigger = lock.GetBody().IsSensor();
    }
    return hit;
}

PhysicsTransform blend(const PhysicsTransform& from, const PhysicsTransform& to, double t) {
    return PhysicsTransform{glm::mix(from.position, to.position, t), glm::slerp(from.rotation, to.rotation, static_cast<float>(t))};
}

}  // namespace
}  // namespace detail

using detail::PhysicsState;

core::Result<std::unique_ptr<PhysicsWorld>, PhysicsError> PhysicsWorld::create(const PhysicsWorldDesc& desc) {
    // Each body owns an object layer id, which Jolt stores in 16 bits.
    if (desc.max_bodies == 0 || desc.max_bodies > 65535 || desc.max_catch_up_steps == 0 || !detail::finite(desc.gravity) ||
        !detail::finite(desc.origin)) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    detail::initialise_jolt();
    std::unique_ptr<PhysicsWorld> physics_world(new PhysicsWorld());
    physics_world->state_ = std::make_unique<PhysicsState>(desc);
    return core::success(std::move(physics_world));
}

PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld() {
    close();
}

core::Result<ecs::EntityHandle, PhysicsError> PhysicsWorld::add_body(const BodyDesc& desc) {
    PhysicsState& state = *state_;
    if (state.closed) {
        return core::failure(PhysicsError::InvalidState);
    }
    if (!desc.entity.valid() || state.bodies.contains(desc.entity)) {
        return core::failure(PhysicsError::InvalidEntity);
    }
    if (!detail::valid_desc(desc)) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    const JPH::ShapeSettings::ShapeResult shape = detail::make_shape(desc);
    if (shape.HasError()) {
        return core::failure(PhysicsError::InvalidDesc);
    }

    const JPH::EMotionType motion = detail::motion_type(desc.motion);
    const glm::quat rotation = glm::normalize(desc.rotation);
    const JPH::ObjectLayer object_layer = detail::allocate_object_layer(state.table);
    JPH::BodyCreationSettings settings(shape.Get(), detail::to_jolt(detail::to_local(desc.position, state.origin)),
                                       detail::to_jolt(rotation), motion, object_layer);
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mIsSensor = desc.trigger;

    JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    JPH::Body* body = bodies.CreateBody(settings);
    if (body == nullptr) {
        detail::free_object_layer(state.table, object_layer);
        return core::failure(PhysicsError::BodyLimit);
    }
    state.table.entries[object_layer] = detail::make_filter(desc);
    const JPH::BodyID id = body->GetID();
    // Activation events for the new body are dropped: it is not in the entity map until the insert below.
    bodies.AddBody(id, desc.motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

    const PhysicsTransform pose = detail::read_body(bodies, id, state.origin);
    state.bodies.emplace(desc.entity, detail::BodyRecord{id, desc.motion, pose, pose, object_layer});
    state.entity_by_body.emplace(id.GetIndexAndSequenceNumber(), desc.entity);
    return core::success(desc.entity);
}

bool PhysicsWorld::remove_body(ecs::EntityHandle entity) {
    PhysicsState& state = *state_;
    const auto it = state.bodies.find(entity);
    if (state.closed || it == state.bodies.end()) {
        return false;
    }
    detail::remove_constraints_of(state, entity);
    const JPH::BodyID id = it->second.id;
    const JPH::ObjectLayer object_layer = it->second.object_layer;
    JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
    state.entity_by_body.erase(id.GetIndexAndSequenceNumber());
    state.bodies.erase(it);
    detail::free_object_layer(state.table, object_layer);
    return true;
}

bool PhysicsWorld::has_body(ecs::EntityHandle entity) const {
    return !state_->closed && state_->bodies.contains(entity);
}

std::size_t PhysicsWorld::body_count() const {
    return state_->bodies.size();
}

core::Result<ConstraintHandle, PhysicsError> PhysicsWorld::add_constraint(const ConstraintDesc& desc) {
    PhysicsState& state = *state_;
    if (state.closed) {
        return core::failure(PhysicsError::InvalidState);
    }
    if (!detail::valid_constraint(desc)) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    const auto first = state.bodies.find(desc.body_a);
    const auto second = state.bodies.find(desc.body_b);
    if (first == state.bodies.end() || second == state.bodies.end()) {
        return core::failure(PhysicsError::InvalidEntity);
    }
    const JPH::BodyLockInterface& locks = state.system->GetBodyLockInterface();
    JPH::Body* body_a = locks.TryGetBody(first->second.id);
    JPH::Body* body_b = locks.TryGetBody(second->second.id);
    if (body_a == nullptr || body_b == nullptr) {
        return core::failure(PhysicsError::InvalidEntity);
    }

    JPH::Ref<JPH::TwoBodyConstraint> joint = detail::make_constraint(desc, *body_a, *body_b);
    if (joint.GetPtr() == nullptr) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    state.system->AddConstraint(joint.GetPtr());
    detail::wake(state, desc.body_a);
    detail::wake(state, desc.body_b);
    const ConstraintHandle handle{state.next_constraint++};
    state.constraints.emplace(handle.value, detail::ConstraintRecord{desc.body_a, desc.body_b, std::move(joint)});
    return core::success(handle);
}

bool PhysicsWorld::remove_constraint(ConstraintHandle handle) {
    PhysicsState& state = *state_;
    const auto it = state.constraints.find(handle.value);
    if (state.closed || it == state.constraints.end()) {
        return false;
    }
    state.system->RemoveConstraint(it->second.joint.GetPtr());
    detail::wake(state, it->second.body_a);
    detail::wake(state, it->second.body_b);
    state.constraints.erase(it);
    return true;
}

std::size_t PhysicsWorld::constraint_count() const {
    return state_->constraints.size();
}

bool PhysicsWorld::move_kinematic(ecs::EntityHandle entity, const glm::dvec3& position, const glm::quat& rotation) {
    PhysicsState& state = *state_;
    const auto it = state.bodies.find(entity);
    if (state.closed || it == state.bodies.end() || it->second.motion != BodyMotion::Kinematic) {
        return false;
    }
    if (!detail::finite(position) || !detail::finite(rotation) || glm::length(rotation) <= 0.0f) {
        return false;
    }
    JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    bodies.MoveKinematic(it->second.id, detail::to_jolt(detail::to_local(position, state.origin)), detail::to_jolt(glm::normalize(rotation)),
                         static_cast<float>(kFixedStep));
    return true;
}

bool PhysicsWorld::set_linear_velocity(ecs::EntityHandle entity, const glm::vec3& velocity) {
    PhysicsState& state = *state_;
    const auto it = state.bodies.find(entity);
    if (state.closed || it == state.bodies.end() || it->second.motion != BodyMotion::Dynamic || !detail::finite3(velocity)) {
        return false;
    }
    JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    bodies.SetLinearVelocity(it->second.id, detail::to_jolt(velocity));
    bodies.ActivateBody(it->second.id);
    return true;
}

std::size_t PhysicsWorld::active_body_count() const {
    const PhysicsState& state = *state_;
    if (state.closed || !state.system) {
        return 0;
    }
    return state.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

core::Result<ecs::EntityHandle, PhysicsError> PhysicsWorld::replace_collider(ecs::EntityHandle entity, const BodyDesc& desc) {
    PhysicsState& state = *state_;
    if (state.closed) {
        return core::failure(PhysicsError::InvalidState);
    }
    const auto it = state.bodies.find(entity);
    if (it == state.bodies.end() || desc.entity != entity) {
        return core::failure(PhysicsError::InvalidEntity);
    }
    if (desc.motion != it->second.motion || !detail::valid_desc(desc)) {
        return core::failure(PhysicsError::InvalidDesc);
    }
    const JPH::ShapeSettings::ShapeResult shape = detail::make_shape(desc);
    if (shape.HasError()) {
        return core::failure(PhysicsError::InvalidDesc);
    }

    const detail::BodyRecord& record = it->second;
    JPH::BodyInterface& bodies = state.system->GetBodyInterface();
    bodies.SetShape(record.id, shape.Get(), true,
                    record.motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    bodies.SetIsSensor(record.id, desc.trigger);
    state.table.entries[record.object_layer] = detail::make_filter(desc);
    return core::success(entity);
}

std::optional<QueryHit> PhysicsWorld::raycast(const glm::dvec3& origin, const glm::vec3& direction, float max_distance,
                                              const QueryFilter& filter) const {
    const PhysicsState& state = *state_;
    if (state.closed || !detail::finite(origin) || !std::isfinite(max_distance) || max_distance <= 0.0f || !std::isfinite(direction.x) ||
        !std::isfinite(direction.y) || !std::isfinite(direction.z) || glm::length(direction) <= 0.0f) {
        return std::nullopt;
    }
    const glm::vec3 unit = glm::normalize(direction);
    const JPH::RRayCast ray(detail::to_jolt(detail::to_local(origin, state.origin)), JPH::Vec3(unit.x, unit.y, unit.z) * max_distance);

    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    const auto ignored = state.bodies.find(filter.ignore);
    const JPH::BodyID ignored_id = ignored != state.bodies.end() ? ignored->second.id : JPH::BodyID();
    state.system->GetNarrowPhaseQuery().CastRay(ray, JPH::RayCastSettings(), collector, {}, {},
                                                detail::QueryBodies(state.table, filter, ignored_id));
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    const JPH::RayCastResult& hit = collector.mHit;
    return detail::describe(state, hit.mBodyID, hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction), hit.mFraction * max_distance);
}

std::optional<QueryHit> PhysicsWorld::shape_cast(const BodyDesc& shape_desc, const glm::vec3& direction, float max_distance,
                                                 const QueryFilter& filter) const {
    const PhysicsState& state = *state_;
    if (state.closed || !detail::valid_desc(shape_desc) || !std::isfinite(max_distance) || max_distance <= 0.0f ||
        glm::length(direction) <= 0.0f || !std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z)) {
        return std::nullopt;
    }
    const JPH::ShapeSettings::ShapeResult shape = detail::make_shape(shape_desc);
    if (shape.HasError()) {
        return std::nullopt;
    }
    const glm::vec3 unit = glm::normalize(direction);
    const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(detail::to_jolt(glm::normalize(shape_desc.rotation)),
                                                               detail::to_jolt(detail::to_local(shape_desc.position, state.origin)));
    const JPH::RShapeCast cast(shape.Get(), JPH::Vec3::sReplicate(1.0f), start, JPH::Vec3(unit.x, unit.y, unit.z) * max_distance);

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    state.system->GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings(), JPH::RVec3::sZero(), collector, {}, {},
                                                  detail::QueryBodies(state.table, filter, JPH::BodyID()));
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    const JPH::ShapeCastResult& hit = collector.mHit;
    QueryHit result = detail::describe(state, hit.mBodyID2, hit.mSubShapeID2, hit.mContactPointOn2, hit.mFraction * max_distance);
    // The normal is the cast's contact normal, not the body's surface normal there. At an edge the surface normal picks one face, so a
    // capsule touching the edge of a step would read a wall as a floor.
    const JPH::Vec3 contact = -hit.mPenetrationAxis;
    if (contact.LengthSq() > 0.0f) {
        result.normal = glm::normalize(glm::vec3(contact.GetX(), contact.GetY(), contact.GetZ()));
    }
    return result;
}

bool PhysicsWorld::has_moving_body_in(const glm::dvec3& min, const glm::dvec3& max) const {
    if (state_->closed) {
        return false;
    }
    return std::any_of(state_->bodies.begin(), state_->bodies.end(), [&](const auto& entry) {
        const detail::BodyRecord& record = entry.second;
        const glm::dvec3& p = record.current.position;
        return record.motion != BodyMotion::Static && p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z &&
               p.z <= max.z;
    });
}

std::vector<ecs::EntityHandle> PhysicsWorld::overlaps(const BodyDesc& shape_desc, const QueryFilter& filter) const {
    const PhysicsState& state = *state_;
    std::vector<ecs::EntityHandle> found;
    if (state.closed || !detail::valid_desc(shape_desc)) {
        return found;
    }
    const JPH::ShapeSettings::ShapeResult shape = detail::make_shape(shape_desc);
    if (shape.HasError()) {
        return found;
    }
    const JPH::RMat44 centre = JPH::RMat44::sRotationTranslation(detail::to_jolt(glm::normalize(shape_desc.rotation)),
                                                                 detail::to_jolt(detail::to_local(shape_desc.position, state.origin)));
    const auto ignored = state.bodies.find(filter.ignore);
    const JPH::BodyID ignored_id = ignored != state.bodies.end() ? ignored->second.id : JPH::BodyID();

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    state.system->GetNarrowPhaseQuery().CollideShape(shape.Get(), JPH::Vec3::sReplicate(1.0f), centre, JPH::CollideShapeSettings(),
                                                     JPH::RVec3::sZero(), collector, {}, {},
                                                     detail::QueryBodies(state.table, filter, ignored_id));
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        const auto entity = state.entity_by_body.find(hit.mBodyID2.GetIndexAndSequenceNumber());
        if (entity != state.entity_by_body.end()) {
            found.push_back(entity->second);
        }
    }
    return found;
}

core::Status PhysicsWorld::update(double frame_seconds) {
    if (state_->closed) {
        return {core::StatusCode::InvalidState};
    }
    ENGINE_ASSERT(std::isfinite(frame_seconds) && frame_seconds >= 0.0, "frame time must be finite and not negative");
    return detail::update_state(*state_, frame_seconds);
}

void PhysicsWorld::set_paused(bool paused) noexcept {
    state_->paused = paused;
}

bool PhysicsWorld::paused() const noexcept {
    return state_->paused;
}

double PhysicsWorld::interpolation_alpha() const noexcept {
    return state_->alpha;
}

std::optional<PhysicsTransform> PhysicsWorld::step_transform(ecs::EntityHandle entity) const {
    const auto it = state_->bodies.find(entity);
    if (state_->closed || it == state_->bodies.end()) {
        return std::nullopt;
    }
    return it->second.current;
}

std::optional<PhysicsTransform> PhysicsWorld::render_transform(ecs::EntityHandle entity) const {
    const auto it = state_->bodies.find(entity);
    if (state_->closed || it == state_->bodies.end()) {
        return std::nullopt;
    }
    return detail::blend(it->second.previous, it->second.current, state_->alpha);
}

std::vector<PhysicsEvent> PhysicsWorld::take_events() {
    if (state_->closed) {
        return {};
    }
    return state_->callbacks->take();
}

PhysicsStats PhysicsWorld::stats() const {
    return state_->stats;
}

glm::dvec3 PhysicsWorld::origin() const {
    return state_->origin;
}

std::shared_ptr<world::OriginSubscriber> PhysicsWorld::origin_subscriber() {
    return state_->link;
}

void PhysicsWorld::close() {
    detail::close_state(*state_);
}

bool PhysicsWorld::closed() const noexcept {
    return state_->closed;
}

}  // namespace engine::physics
