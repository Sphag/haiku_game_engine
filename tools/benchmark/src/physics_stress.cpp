#include "engine/benchmark/physics_stress.hpp"

#include "engine/benchmark/measure.hpp"
#include "engine/benchmark/workload.hpp"
#include "engine/ecs/world.hpp"
#include "engine/physics/character.hpp"
#include "engine/physics/physics_world.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine::bench::physics {
namespace {

namespace phys = engine::physics;

constexpr std::uint64_t kStepsPerSecond = 60;  // the fixed rate; kFixedRateHz is 60
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kGroundThicknessM = 0.5;
constexpr std::uint64_t kKickPeriodFrames = 30;   // each body is kicked every half second, staggered by its index
constexpr double kKickSpreadMps = 4.0;            // horizontal kick speed range, centred on zero
constexpr double kKickLiftMinMps = 2.0;           // vertical kick speed range
constexpr double kKickLiftSpreadMps = 2.0;
constexpr double kWalkSpeedMps = 1.5;
constexpr std::uint64_t kJumpPeriodFrames = 180;  // a character may jump every 3 seconds, staggered by its index
constexpr std::uint64_t kRaycastsPerSecond = 1000;
constexpr std::uint64_t kShapeCastsPerSecond = 100;
constexpr double kQuerySpreadM = 120.0;           // query origins fall within this square around the world centre
constexpr double kQueryHeightM = 80.0;
constexpr double kRaycastMaxM = 200.0;
constexpr double kShapeCastMaxM = 100.0;
constexpr float kProbeRadiusM = 0.25f;

constexpr std::uint64_t kSaltKickX = 11;
constexpr std::uint64_t kSaltKickY = 12;
constexpr std::uint64_t kSaltKickZ = 13;
constexpr std::uint64_t kSaltWalk = 14;
constexpr std::uint64_t kSaltRayX = 15;
constexpr std::uint64_t kSaltRayZ = 16;
constexpr std::uint64_t kSaltCastX = 17;
constexpr std::uint64_t kSaltCastZ = 18;

// SplitMix64, the same mixer the workload generator uses. The schedule draws every random value from it, so a run is repeatable.
std::uint64_t mix(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// A value in [0, 1) from a counter and a salt. Built from the top 53 bits, so the conversion is exact.
double unit(std::uint64_t counter, std::uint64_t salt) {
    constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^-53
    return static_cast<double>(mix(counter * 0x9E3779B97F4A7C15ull + salt) >> 11) * kScale;
}

glm::dvec3 to_metres(const std::array<std::int64_t, 3>& mm) {
    return glm::dvec3(static_cast<double>(mm[0]), static_cast<double>(mm[1]), static_cast<double>(mm[2])) / 1000.0;
}

glm::vec3 half_metres(const std::array<std::uint32_t, 3>& mm) {
    return glm::vec3(static_cast<float>(mm[0]), static_cast<float>(mm[1]), static_cast<float>(mm[2])) / 1000.0f;
}

// The velocity a body is kicked with at a frame: a horizontal push and an upward lift, both from the seed.
glm::vec3 kick_velocity(std::uint64_t frame, std::uint64_t body) {
    const std::uint64_t counter = frame * 4096 + body;
    const auto horizontal_x = static_cast<float>((unit(counter, kSaltKickX) - 0.5) * kKickSpreadMps);
    const auto lift = static_cast<float>(kKickLiftMinMps + unit(counter, kSaltKickY) * kKickLiftSpreadMps);
    const auto horizontal_z = static_cast<float>((unit(counter, kSaltKickZ) - 0.5) * kKickSpreadMps);
    return glm::vec3(horizontal_x, lift, horizontal_z);
}

bool finite(const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const glm::quat& q) {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

// True when a finite position lies inside the reference bounds around the world centre.
bool inside_reference(const glm::dvec3& p, double centre_x, double centre_z) {
    return std::abs(p.x - centre_x) <= kReferenceHalfExtentM && std::abs(p.z - centre_z) <= kReferenceHalfExtentM &&
           p.y >= kReferenceMinHeightM && p.y <= kReferenceMaxHeightM;
}

// Counts one sample of a body or character: a missing or non-finite pose is a NaN sample, and a finite one outside the bounds is an
// out-of-bounds sample.
void check_position(const glm::dvec3* position, bool pose_finite, double centre_x, double centre_z, Result& out) {
    if (position == nullptr || !pose_finite || !finite(*position)) {
        ++out.nan_transforms;
        return;
    }
    if (!inside_reference(*position, centre_x, centre_z)) {
        ++out.out_of_bounds_samples;
    }
}

core::Result<Result, std::string> fail(const std::string& message) {
    return core::failure(message);
}

}  // namespace

core::Result<Result, std::string> run(const SceneDef& scene, const RunSettings& settings) {
    auto generated = generate_physics_spec("physics_stress", scene);
    if (!generated.ok()) {
        return core::failure(generated.error());
    }
    const PhysicsSpec spec = std::move(generated).value();
    const double centre_x = static_cast<double>(spec.centre_mm) / 1000.0;
    const double centre_z = centre_x;

    phys::PhysicsWorldDesc desc;
    desc.worker_threads = settings.worker_threads;
    auto made_world = phys::PhysicsWorld::create(desc);
    if (!made_world.ok()) {
        return fail("the physics world could not be created");
    }
    const std::unique_ptr<phys::PhysicsWorld> world = std::move(made_world).value();

    ecs::World entities;
    Result out;

    // The ground under the scene, sized to the reference bounds. It is not part of the table.
    phys::BodyDesc ground;
    ground.entity = entities.create();
    ground.motion = phys::BodyMotion::Static;
    ground.shape = phys::ShapeKind::Box;
    ground.half_extents = glm::vec3(static_cast<float>(kReferenceHalfExtentM), static_cast<float>(kGroundThicknessM),
                                    static_cast<float>(kReferenceHalfExtentM));
    ground.position = glm::dvec3(centre_x, -kGroundThicknessM, centre_z);
    if (!world->add_body(ground).ok()) {
        return fail("the ground body was rejected");
    }
    out.created.ground = 1;

    std::vector<ecs::EntityHandle> bodies;
    std::vector<glm::dvec3> body_positions;
    bodies.reserve(spec.bodies.size());
    for (const PhysicsBodySpec& body : spec.bodies) {
        phys::BodyDesc body_desc;
        body_desc.entity = entities.create();
        body_desc.motion = phys::BodyMotion::Dynamic;
        body_desc.shape = phys::ShapeKind::Box;
        body_desc.half_extents = half_metres(body.half_extent_mm);
        body_desc.position = to_metres(body.position_mm);
        if (!world->add_body(body_desc).ok()) {
            return fail("a dynamic body was rejected");
        }
        bodies.push_back(body_desc.entity);
        body_positions.push_back(body_desc.position);
    }
    for (const PhysicsStaticSpec& box : spec.statics) {
        phys::BodyDesc static_desc;
        static_desc.entity = entities.create();
        static_desc.motion = phys::BodyMotion::Static;
        static_desc.shape = phys::ShapeKind::Box;
        static_desc.half_extents = half_metres(box.half_extent_mm);
        static_desc.position = to_metres(box.position_mm);
        if (!world->add_body(static_desc).ok()) {
            return fail("a static collider was rejected");
        }
    }

    // Joints cycle through the three kinds. Each anchor pair is placed where the two bodies already are, so a joint starts with no
    // correction. Fixed and hinge weld body a's centre to a point on body b; a distance joint keeps the two centres at their current gap.
    constexpr phys::ConstraintKind kinds[] = {phys::ConstraintKind::Fixed, phys::ConstraintKind::Hinge, phys::ConstraintKind::Distance};
    for (std::size_t c = 0; c < spec.constraints.size(); ++c) {
        const PhysicsConstraintSpec& link = spec.constraints[c];
        const glm::dvec3 offset = body_positions[link.body_a] - body_positions[link.body_b];
        phys::ConstraintDesc joint;
        joint.kind = kinds[c % 3];
        joint.body_a = bodies[link.body_a];
        joint.body_b = bodies[link.body_b];
        if (joint.kind == phys::ConstraintKind::Distance) {
            joint.max_distance = static_cast<float>(std::max(glm::length(offset), 0.1));
        } else {
            joint.anchor_b = glm::vec3(offset);
        }
        if (!world->add_constraint(joint).ok()) {
            return fail("a constraint was rejected");
        }
    }

    std::vector<phys::CharacterController> characters;
    characters.reserve(spec.characters.size());
    for (const PhysicsCharacterSpec& character : spec.characters) {
        phys::CharacterDesc character_desc;
        character_desc.position = to_metres(character.position_mm);
        character_desc.radius = static_cast<float>(character.radius_mm) / 1000.0f;
        auto made = phys::CharacterController::create(character_desc);
        if (!made.ok()) {
            return fail("a physics character was rejected");
        }
        characters.push_back(std::move(made).value());
    }

    out.created.dynamic_bodies = bodies.size();
    out.created.static_colliders = spec.statics.size();
    out.created.constraints = spec.constraints.size();
    out.created.characters = characters.size();
    if (world->body_count() != bodies.size() + spec.statics.size() + out.created.ground) {
        return fail("the world does not hold every body the scene created");
    }
    if (world->constraint_count() != spec.constraints.size()) {
        return fail("the world does not hold every constraint the scene created");
    }

    const auto warmup_frames = static_cast<std::uint64_t>(std::llround(settings.warmup_s * static_cast<double>(kStepsPerSecond)));
    const auto measured_window = static_cast<std::uint64_t>(std::llround(settings.duration_s * static_cast<double>(kStepsPerSecond)));
    const std::uint64_t total_frames = warmup_frames + measured_window;
    std::uint64_t active_sum = 0;
    const auto dynamic_count = static_cast<std::uint64_t>(bodies.size());

    for (std::uint64_t frame = 0; frame < total_frames; ++frame) {
        const bool measured = frame >= warmup_frames;
        const double dt = settings.frame_seconds ? settings.frame_seconds(frame) : phys::kFixedStep;
        const auto started = std::chrono::steady_clock::now();

        // The action sequence: kicks, then character moves, then the queries that look at the bodies as they are before the step.
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (i % kKickPeriodFrames == frame % kKickPeriodFrames &&
                world->set_linear_velocity(bodies[i], kick_velocity(frame, i))) {
                ++out.interactions;
            }
        }
        const double heading_base = static_cast<double>(frame) * phys::kFixedStep * 0.5;
        for (std::size_t c = 0; c < characters.size(); ++c) {
            const double heading = kTwoPi * unit(c, kSaltWalk) + heading_base;
            const glm::vec3 walk(static_cast<float>(std::cos(heading) * kWalkSpeedMps), 0.0f,
                                 static_cast<float>(std::sin(heading) * kWalkSpeedMps));
            const bool jump = frame % kJumpPeriodFrames == (c * 9) % kJumpPeriodFrames;
            if (!characters[c].update(*world, walk, jump, dt).ok()) {
                ++out.update_errors;
            }
        }

        const std::uint64_t ray_due = (frame + 1) * kRaycastsPerSecond / kStepsPerSecond;
        for (; out.raycasts < ray_due; ++out.raycasts) {
            const std::uint64_t q = out.raycasts;
            const glm::dvec3 origin(centre_x + (unit(q, kSaltRayX) - 0.5) * kQuerySpreadM, kQueryHeightM,
                                    centre_z + (unit(q, kSaltRayZ) - 0.5) * kQuerySpreadM);
            if (world->raycast(origin, glm::vec3(0.0f, -1.0f, 0.0f), static_cast<float>(kRaycastMaxM))) {
                ++out.raycast_hits;
            }
        }
        const std::uint64_t cast_due = (frame + 1) * kShapeCastsPerSecond / kStepsPerSecond;
        for (; out.shape_casts < cast_due; ++out.shape_casts) {
            const std::uint64_t q = out.shape_casts;
            phys::BodyDesc probe;
            probe.motion = phys::BodyMotion::Dynamic;
            probe.shape = phys::ShapeKind::Sphere;
            probe.radius = kProbeRadiusM;
            probe.position = glm::dvec3(centre_x + (unit(q, kSaltCastX) - 0.5) * kQuerySpreadM, kQueryHeightM * 0.75,
                                        centre_z + (unit(q, kSaltCastZ) - 0.5) * kQuerySpreadM);
            if (world->shape_cast(probe, glm::vec3(0.0f, -1.0f, 0.0f), static_cast<float>(kShapeCastMaxM))) {
                ++out.shape_cast_hits;
            }
        }

        const core::Status stepped = world->update(dt);
        if (!stepped.ok()) {
            return fail("the physics world was closed during the run");
        }
        const auto finished = std::chrono::steady_clock::now();
        if (measured) {
            out.frame_ms.push_back(std::chrono::duration<double, std::milli>(finished - started).count());
        }

        for (const phys::PhysicsEvent& event : world->take_events()) {
            switch (event.kind) {
                case phys::PhysicsEventKind::ContactAdded: ++out.contact_events; break;
                case phys::PhysicsEventKind::BodyActivated: ++out.activation_events; break;
                case phys::PhysicsEventKind::BodyDeactivated: ++out.deactivation_events; break;
                case phys::PhysicsEventKind::ContactRemoved: break;
            }
        }

        // The checks read every body after the step. They run outside the timed work, so they do not change the frame cost.
        const auto active = static_cast<std::uint64_t>(world->active_body_count());
        const std::uint64_t sleeping = dynamic_count > active ? dynamic_count - active : 0;
        if (measured) {
            if (out.measured_frames == 0) {
                out.active_min = out.active_max = active;
                out.sleeping_min = out.sleeping_max = sleeping;
            } else {
                out.active_min = std::min(out.active_min, active);
                out.active_max = std::max(out.active_max, active);
                out.sleeping_min = std::min(out.sleeping_min, sleeping);
                out.sleeping_max = std::max(out.sleeping_max, sleeping);
            }
            active_sum += active;
            ++out.measured_frames;
        }
        for (const ecs::EntityHandle& entity : bodies) {
            const auto transform = world->step_transform(entity);
            if (!transform) {
                check_position(nullptr, false, centre_x, centre_z, out);
            } else {
                const glm::dvec3 position = transform->position;
                check_position(&position, finite(transform->rotation), centre_x, centre_z, out);
            }
        }
        for (const phys::CharacterController& character : characters) {
            const glm::dvec3 feet = character.position();
            check_position(&feet, true, centre_x, centre_z, out);
        }
        ++out.frames;
    }

    const phys::PhysicsStats stats = world->stats();
    out.simulation_steps = stats.steps;
    out.catch_up_events = stats.catch_up_events;
    out.dropped_steps = stats.dropped_steps;
    out.update_errors += stats.update_errors;
    out.active_mean = out.measured_frames == 0 ? 0.0 : static_cast<double>(active_sum) / static_cast<double>(out.measured_frames);
    return core::success(out);
}

std::vector<std::string> failures(const Result& result) {
    std::vector<std::string> reasons;
    if (result.dropped_steps > 0) {
        reasons.push_back("lost timesteps: " + std::to_string(result.dropped_steps) + " fixed steps were dropped by the catch-up limit");
    }
    if (result.update_errors > 0) {
        reasons.push_back(std::to_string(result.update_errors) + " physics steps or character moves reported an error");
    }
    if (result.nan_transforms > 0) {
        reasons.push_back(std::to_string(result.nan_transforms) + " body or character samples had a missing or non-finite transform");
    }
    if (result.out_of_bounds_samples > 0) {
        reasons.push_back(std::to_string(result.out_of_bounds_samples) + " body or character samples left the reference bounds");
    }
    return reasons;
}

void run_physics_stress(const SceneDef& scene, const RunSettings& settings, RunRecord& record) {
    const auto measured = run(scene, settings);
    if (!measured.ok()) {
        record.status = RunStatus::Incomplete;
        record.reason = measured.error();
        return;
    }
    const Result& result = measured.value();

    record.frames_started = result.frames;
    record.frames.cpu_ms = result.frame_ms;
    record.counters = {
        {"bodies_dynamic", result.created.dynamic_bodies},
        {"bodies_static", result.created.static_colliders},
        {"bodies_ground", result.created.ground},
        {"constraints", result.created.constraints},
        {"characters", result.created.characters},
        {"simulation_steps", result.simulation_steps},
        {"measured_frames", result.measured_frames},
        {"catch_up_events", result.catch_up_events},
        {"lost_timesteps", result.dropped_steps},
        {"update_errors", result.update_errors},
        {"active_bodies_min", result.active_min},
        {"active_bodies_max", result.active_max},
        {"active_bodies_mean", static_cast<std::uint64_t>(std::llround(result.active_mean))},
        {"sleeping_bodies_min", result.sleeping_min},
        {"sleeping_bodies_max", result.sleeping_max},
        {"interactions", result.interactions},
        {"raycasts", result.raycasts},
        {"raycast_hits", result.raycast_hits},
        {"shape_casts", result.shape_casts},
        {"shape_cast_hits", result.shape_cast_hits},
        {"contact_events", result.contact_events},
        {"activation_events", result.activation_events},
        {"deactivation_events", result.deactivation_events},
        {"nan_transforms", result.nan_transforms},
        {"out_of_bounds_samples", result.out_of_bounds_samples},
    };

    if (result.frame_ms.empty()) {
        record.status = RunStatus::Incomplete;
        record.reason = "no frames were measured";
        return;
    }
    record.cpu = summarize(result.frame_ms);

    const std::vector<std::string> reasons = failures(result);
    if (!reasons.empty()) {
        record.status = RunStatus::Failed;
        record.reason.clear();
        for (const std::string& reason : reasons) {
            record.reason += (record.reason.empty() ? "" : "; ") + reason;
        }
        return;
    }
    record.status = RunStatus::Passed;
    record.reason = "headless diagnostic; no frame-time contract; no lost timestep, non-finite transform, or out-of-bounds body";
}

}  // namespace engine::bench::physics
