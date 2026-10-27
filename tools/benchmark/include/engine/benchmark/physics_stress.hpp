#pragma once

// physics_stress (US-077): the headless physics benchmark. It builds the table's dynamic bodies, static colliders, constraints, and
// physics characters from the seeded workload (generate_physics_spec), runs them at the fixed 60 Hz step along a fixed action
// sequence, and reports the simulation cost. A run is failed when the physics lost a timestep, produced a non-finite transform, or
// moved a body or character outside the reference bounds. The run uses simulated time, so the action sequence and the queries are the
// same on every host; only the measured frame times depend on the machine.
//
// The action sequence (physics_action_sequence_v1, recorded in benchmarks/manifest.json):
//   - every dynamic body is given a velocity kick every 30 frames, staggered by its index, so none is left to sleep;
//   - every physics character walks in a circle at 1.5 m/s and jumps on a staggered 3-second period;
//   - raycasts and shape casts are issued at 1000/s and 100/s, from positions drawn from the seed.
// This file is part of the benchmark library. It needs no GPU, so the same run works on every operating system.

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/report.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine::bench::physics {

struct RunSettings {
    double warmup_s = 0.0;   // frames run before the measured window; they are simulated but not timed
    double duration_s = 0.0;  // the measured window
    std::uint32_t worker_threads = 1;  // Jolt job threads; zero runs every job on the calling thread
    // Frame time the driver passes to each update, by frame index. Empty means the fixed step every frame. A test passes a longer frame
    // to make the catch-up limit drop steps, which is how a lost timestep shows up.
    std::function<double(std::uint64_t frame)> frame_seconds;
};

// The scene as it was created in the world. The ground is one static box under the scene (not part of the table).
struct Created {
    std::uint64_t dynamic_bodies = 0;
    std::uint64_t static_colliders = 0;
    std::uint64_t ground = 0;
    std::uint64_t constraints = 0;
    std::uint64_t characters = 0;
};

// Everything one run measured. Counts cover the whole run (warmup included) unless the field says it is measured.
struct Result {
    Created created;
    std::uint64_t frames = 0;
    std::uint64_t measured_frames = 0;
    std::uint64_t simulation_steps = 0;     // fixed steps the world ran
    std::uint64_t catch_up_events = 0;      // updates that reached the catch-up limit with time left over
    std::uint64_t dropped_steps = 0;        // lost timesteps: fixed steps the catch-up limit discarded
    std::uint64_t update_errors = 0;        // steps Jolt or a character reported as failed
    std::uint64_t active_min = 0;           // awake dynamic bodies, over the measured frames
    std::uint64_t active_max = 0;
    double active_mean = 0.0;
    std::uint64_t sleeping_min = 0;         // dynamic bodies at rest, over the measured frames
    std::uint64_t sleeping_max = 0;
    std::uint64_t interactions = 0;         // velocity kicks applied to bodies
    std::uint64_t raycasts = 0;
    std::uint64_t raycast_hits = 0;
    std::uint64_t shape_casts = 0;
    std::uint64_t shape_cast_hits = 0;
    std::uint64_t contact_events = 0;
    std::uint64_t activation_events = 0;
    std::uint64_t deactivation_events = 0;
    std::uint64_t nan_transforms = 0;       // body or character samples that were missing or not finite
    std::uint64_t out_of_bounds_samples = 0;  // body or character samples outside the reference bounds
    std::vector<double> frame_ms;           // measured frames: wall time of the frame's simulation work
};

// The reference bounds: a horizontal reach of 400 m from the world centre, and heights from -50 m to 400 m. A body that leaves them has
// been thrown out of the scene, which the benchmark counts as a failure.
inline constexpr double kReferenceHalfExtentM = 400.0;
inline constexpr double kReferenceMinHeightM = -50.0;
inline constexpr double kReferenceMaxHeightM = 400.0;

// Runs the scene once. Fails when the scene's workload cannot be generated or the world rejects a body or joint; a run that completes is
// returned whatever it measured, and failures() judges it.
[[nodiscard]] core::Result<Result, std::string> run(const SceneDef& scene, const RunSettings& settings);

// The reasons a run failed, one per check that tripped: lost timesteps, Jolt or character step errors, non-finite transforms, and
// samples outside the reference bounds. An empty list means the run is clean.
[[nodiscard]] std::vector<std::string> failures(const Result& result);

// Measures physics_stress once and fills `record` (status, reason, frame times, and the counters the report carries). The headless profile
// has no frame-time contract, so a complete clean run passes; a run with a failure check is failed; an incomplete one is incomplete.
void run_physics_stress(const SceneDef& scene, const RunSettings& settings, RunRecord& record);

}  // namespace engine::bench::physics
