#pragma once

// streaming_stress (US-078): the streaming benchmark on the CPU. It writes the shared world's cells from the seeded workload (the table's
// descriptors, distributed over 32 x 32 cells of 256 m), then drives StreamingManager along streaming_route_v1: a 5400 m route walked at
// 30 m/s, forward and then back, so the camera crosses cell boundaries and backtracks. A separate teleport case moves the camera from one
// end of the route to the other and checks the window it lands in. Each run records the loaded and unloaded cells, the IO, decode, and
// upload work per stage, the residency and memory peaks, and the wall time of every frame. Frames run on the fixed 60 Hz simulated step,
// and the route advances in simulated time, so the same route is walked on every host.
//
// Correctness checks, which fail the run:
//   - a cell whose file is missing or does not parse (the manager ends it Failed);
//   - a loaded cell whose object count differs from the authored one, or an object reference that names no object in the world;
//   - a cell inside the active window that is Unloaded or Failed at a sampled frame (its data went missing);
//   - an Active cell outside the active window (an unload that stalled);
//   - a teleport that does not settle on exactly the active window of the new camera cell.
// The soak judges stability: the peak residency of the last traversal must not exceed the first full traversal, so a leak shows up as
// growth. A run shorter than three traversals cannot judge it and is reported incomplete.
//
// The benchmark library holds no GPU types. The GPU residency of the streamed cells is an estimate from the manager (uploaded bytes);
// the full run with a renderer is not part of this build, so the GPU profiles of this scene report missing.

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/report.hpp"
#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::bench::streaming {

// The shared world (manifest "world" block): cells of 256 m, and the world centre the route is built around.
inline constexpr double kRouteCentreM = 4096.0;
inline constexpr double kFixedStepHz = 60.0;

// Residency and memory budgets of the combined scene (the performance contract). The streaming run applies the same limits to its peaks.
inline constexpr std::uint64_t kGpuBudgetBytes = 12ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kHostBudgetBytes = 16ull * 1024ull * 1024ull * 1024ull;

struct RunSettings {
    double warmup_s = 0.0;    // simulated seconds before the measured window; they run but their frames are not timed
    double duration_s = 0.0;  // the measured window
    bool paced = false;       // sleep to the 60 Hz step in wall time; off, frames run as fast as the stream allows
    std::uint32_t io_threads = 4;
};

// streaming_route_v1: waypoints relative to the world centre. The leg lengths sum to the table's route_length_m.
struct Route {
    std::vector<glm::dvec2> waypoints;  // absolute world XZ, in metres
    double length_m = 0.0;
    // The point `distance` metres along the route, clamped to its ends.
    [[nodiscard]] glm::dvec2 at(double distance) const;
};

// The route the table names (streaming_route_v1), checked against the table's route_length_m.
[[nodiscard]] core::Result<Route, std::string> streaming_route(const SceneDef& scene);

// Distance along the route after `travelled` metres of walking: forward over [0, length], then back, then forward again.
[[nodiscard]] double route_distance(double travelled, double length) noexcept;

// One traversal of the route, forward or back: the residency peaks and the stream work inside it.
struct Traversal {
    std::uint64_t index = 0;               // 0 is the first traversal, which starts cold
    std::uint64_t peak_resident_cells = 0;
    std::uint64_t peak_resident_cpu_bytes = 0;
    std::uint64_t loads = 0;
    std::uint64_t unloads = 0;
    bool complete = false;                 // the camera reached the end of the traversal
};

struct Measurement {
    std::uint64_t frames = 0;              // every simulated frame, warmup included
    std::uint64_t measured_frames = 0;
    std::vector<double> frame_ms;          // measured: wall time between the starts of consecutive frames
    std::vector<double> update_ms;         // measured: update() wall time, the IO, decode, and upload dispatch
    std::vector<double> safe_point_ms;     // measured: safe_point() wall time, the activation and unload work
    std::uint64_t io_bytes = 0;            // measured: summed per-frame stage bytes
    std::uint64_t decode_bytes = 0;
    std::uint64_t gpu_upload_bytes = 0;
    std::uint64_t window_samples = 0;      // frames at which the active window was checked
    std::uint64_t missing_window_cells = 0;  // window cells Unloaded or Failed at a sample
    std::uint64_t stale_active_cells = 0;    // Active cells outside the active window at a sample
    std::uint64_t corrupt_loads = 0;       // loaded cells whose object count differs from the authored count
    std::uint64_t dangling_references = 0;  // object references that name no object in the world
    std::uint64_t cells_loaded = 0;
    std::uint64_t cells_unloaded = 0;
    std::uint64_t failed_cells = 0;
    std::uint64_t cancelled_requests = 0;
    std::uint64_t activations = 0;
    std::uint64_t deactivations = 0;
    std::uint64_t teleports = 0;
    std::uint64_t peak_resident_cells = 0;
    std::uint64_t peak_resident_cpu_bytes = 0;
    std::uint64_t peak_gpu_bytes = 0;
    std::uint64_t peak_transient_bytes = 0;
    std::vector<Traversal> traversals;
};

// The teleport correctness case: the camera jumps from the route start to a distant corner and the stream settles on the new window.
struct TeleportResult {
    bool settled = false;                  // the window was Active within the round limit
    double settle_ms = 0.0;                // wall time from the jump to the settled window (reported separately, not an FPS value)
    double worst_frame_ms = 0.0;           // the longest update plus safe point while it settled
    std::uint64_t window_cells = 0;        // authored cells in the new window
    std::uint64_t active_cells = 0;        // Active cells after settling
    std::uint64_t stale_active_cells = 0;  // Active cells that the new window does not contain
    std::uint64_t missing_cells = 0;       // window cells that are not Active after settling
    std::uint64_t teleports = 0;           // the manager's teleport count for the case
    std::uint64_t dangling_references = 0;
    std::uint64_t corrupt_loads = 0;
    std::uint64_t failed_cells = 0;
    std::uint64_t peak_transient_bytes = 0;
};

struct Result {
    std::uint64_t instances = 0;           // descriptors placed into the world
    std::uint64_t cells = 0;               // authored cells that hold at least one descriptor
    std::uint64_t meshes = 0;
    double simulated_s = 0.0;
    Measurement route;
    TeleportResult teleport;
    std::vector<std::string> catalog_problems;  // what validate_world reported; empty for a valid world
};

// Builds the world, runs the route, and runs the teleport case. A failed setup (the table, the route, the world, or the cell files) is an
// error. A run that completes is returned whatever it measured; failures() judges it.
[[nodiscard]] core::Result<Result, std::string> run(const SceneDef& scene, const RunSettings& settings);

// The reasons a result fails its checks, or stability cannot be shown. An empty list with `stability_known` true is a pass.
[[nodiscard]] std::vector<std::string> failures(const Result& result);
[[nodiscard]] bool stability_known(const Result& result);

// Fills one repeat of the record: the counters, the frame times, and the status.
void run_streaming_stress(const SceneDef& scene, const RunSettings& settings, RunRecord& record);

}  // namespace engine::bench::streaming
