#include "engine/benchmark/streaming_stress.hpp"

#include "engine/benchmark/measure.hpp"
#include "engine/benchmark/workload.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/jobs.hpp"
#include "engine/core/path.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace engine::bench::streaming {
namespace {

namespace world = engine::world;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Bytes = core::Guid::Bytes;
using CellKey = std::pair<std::int32_t, std::int32_t>;

constexpr std::uint64_t kTagAsset = 0x41;   // 'A': mesh assets
constexpr std::uint64_t kTagObject = 0x4F;  // 'O': placed descriptors
constexpr double kStepSeconds = 1.0 / kFixedStepHz;
constexpr std::uint64_t kSampleEveryFrames = 60;  // the window is checked once per simulated second
constexpr double kGrowthTolerance = 1.05;         // a later traversal may exceed the first by 5% (allocator and IO timing)
constexpr std::uint64_t kTeleportRoundLimit = 400;

// A GUID whose bytes are fixed by a tag and an index, so the catalog is the same on every run.
Bytes fixed_guid(std::uint64_t tag, std::uint64_t index) {
    Bytes bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(tag >> (56 - 8 * i));
        bytes[static_cast<std::size_t>(8 + i)] = static_cast<std::uint8_t>(index >> (56 - 8 * i));
    }
    return bytes;
}

double ms_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// Removes the scratch directory when the run ends, even if a check failed part way through.
struct ScratchGuard {
    fs::path directory;
    explicit ScratchGuard(fs::path dir) : directory(std::move(dir)) {}
    ~ScratchGuard() {
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
    ScratchGuard(const ScratchGuard&) = delete;
    ScratchGuard& operator=(const ScratchGuard&) = delete;
};

// The authored world the stream reads, and the facts the checks compare the streamed cells against.
struct Catalog {
    world::World world;
    std::set<Bytes> ids;                   // every object GUID in the world
    std::map<CellKey, std::uint64_t> authored;  // descriptors per cell
    std::uint64_t instances = 0;
    std::uint64_t meshes = 0;
    std::vector<std::string> problems;     // validate_world's list; empty for a valid world
    fs::path directory;
};

// Counts the cell lifecycle the checks need. It outlives the manager that calls it (the manager holds a shared copy).
struct Counts {
    std::uint64_t loaded = 0;
    std::uint64_t corrupt = 0;
    std::uint64_t dangling = 0;
};

// Checks each cell when it is decoded: the object count must match the authored cell, and every reference must name an object that
// the world holds. A dangling reference is counted, not repaired, so the report shows it.
class Checker : public world::CellSubscriber {
public:
    Checker(const Catalog& catalog, Counts& counts) : catalog_(catalog), counts_(counts) {}

    void on_loaded(const world::CellData& cell) override {
        ++counts_.loaded;
        const auto found = catalog_.authored.find({cell->coord.x, cell->coord.z});
        const std::uint64_t expected = found == catalog_.authored.end() ? 0 : found->second;
        if (cell->objects.size() != expected) {
            ++counts_.corrupt;
        }
        for (const world::WorldObject& object : cell->objects) {
            for (const core::Guid& reference : object.references) {
                if (catalog_.ids.count(reference.bytes()) == 0) {
                    ++counts_.dangling;
                }
            }
        }
    }

private:
    const Catalog& catalog_;
    Counts& counts_;
};

// The cells of the catalog inside the active window around the camera: the manager's Chebyshev rule, restricted to authored cells.
std::vector<world::CellCoord> window_cells(const Catalog& catalog, glm::dvec2 camera) {
    const world::CellCoord centre = world::cell_coord_at(catalog.world.desc, camera);
    const std::int32_t radius = world::StreamingWindow{}.active_radius;
    std::vector<world::CellCoord> cells;
    for (std::int32_t dz = -radius; dz <= radius; ++dz) {
        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
            const world::CellCoord coord{centre.x + dx, centre.z + dz};
            if (catalog.authored.count({coord.x, coord.z}) != 0) {
                cells.push_back(coord);
            }
        }
    }
    return cells;
}

// Chebyshev distance in cells from the camera's cell, the same measure the window uses.
std::int32_t cell_distance(const world::CellCoord& a, const world::CellCoord& b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z));
}

core::Result<Catalog, std::string> build_catalog(const SceneDef& scene, const fs::path& directory) {
    auto content = generate_render_content(scene);
    if (!content.ok()) {
        return core::failure(content.error());
    }
    auto created = world::create_world(world::WorldDesc{});
    if (!created.ok()) {
        return core::failure(created.error());
    }
    Catalog catalog;
    catalog.world = std::move(created.value());
    catalog.directory = directory;
    catalog.meshes = content.value().meshes.size();

    std::vector<core::Guid> assets;
    for (std::size_t m = 0; m < content.value().meshes.size(); ++m) {
        assets.emplace_back(fixed_guid(kTagAsset, m + 1));
    }

    // The descriptors go straight into their cells. add_object searches every object for a duplicate GUID on each insert, which is
    // quadratic at 200 000 descriptors; the GUIDs here are unique by construction, and validate_world checks the result below.
    const auto& instances = content.value().instances;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const RenderInstance& instance = instances[i];
        world::WorldObject object;
        object.id = core::Guid(fixed_guid(kTagObject, i + 1));
        object.name = "descriptor " + std::to_string(i);
        object.asset = assets[instance.mesh];
        object.position = glm::vec3(static_cast<float>(instance.position[0]), static_cast<float>(instance.position[1]),
                                    static_cast<float>(instance.position[2]));
        const world::CellCoord coord = world::cell_coord_at(catalog.world.desc, glm::dvec2(object.position.x, object.position.z));
        const std::size_t index = world::ensure_cell(catalog.world, coord);
        world::WorldCell& cell = catalog.world.cells[index];
        if (std::find(cell.assets.begin(), cell.assets.end(), object.asset) == cell.assets.end()) {
            cell.assets.push_back(object.asset);
        }
        catalog.ids.insert(object.id.bytes());
        catalog.authored[{coord.x, coord.z}] += 1;
        cell.objects.push_back(std::move(object));
        cell.dirty = true;
        ++catalog.instances;
    }

    // Each cell's first descriptor references the first descriptor of the cell to its east, so the stream has cross-cell references to
    // resolve. A cell that is not resident must not make the reference dangling: the target is named by GUID in the authored world.
    std::map<CellKey, std::size_t> index_of;
    for (std::size_t c = 0; c < catalog.world.cells.size(); ++c) {
        index_of[{catalog.world.cells[c].coord.x, catalog.world.cells[c].coord.z}] = c;
    }
    for (world::WorldCell& cell : catalog.world.cells) {
        const auto east = index_of.find({cell.coord.x + 1, cell.coord.z});
        if (east == index_of.end() || cell.objects.empty() || catalog.world.cells[east->second].objects.empty()) {
            continue;
        }
        cell.objects.front().references.push_back(catalog.world.cells[east->second].objects.front().id);
    }

    catalog.problems = world::validate_world(catalog.world);

    auto path = core::Path::from_utf8(directory.string());
    if (!path.ok()) {
        return core::failure(std::string(core::to_string(path.error())));
    }
    auto saved = world::save_dirty(catalog.world, path.value());
    if (!saved.ok()) {
        return core::failure(saved.error());
    }
    return core::success(std::move(catalog));
}

// The route walk. Each frame is one simulated 60 Hz step; the camera follows the route by simulated distance.
Measurement drive_route(const Catalog& catalog, const Route& route, double speed_mps, const RunSettings& settings) {
    Measurement m;
    Counts counts;
    core::JobSystem jobs(settings.io_threads);
    auto checker = std::make_shared<Checker>(catalog, counts);
    auto path = core::Path::from_utf8(catalog.directory.string());
    world::StreamingManager manager(jobs, path.value(), catalog.world);
    manager.subscribe(checker);

    const auto warmup_frames = static_cast<std::uint64_t>(std::llround(settings.warmup_s * kFixedStepHz));
    const auto total_frames = warmup_frames + static_cast<std::uint64_t>(std::llround(settings.duration_s * kFixedStepHz));
    const double cycle = route.length_m;
    const auto epoch = Clock::now();
    auto previous_start = epoch;

    // Traversal bookkeeping: a traversal is one walk of the route, forward or back, and its peaks come from the manager's stats.
    Traversal current;
    std::uint64_t loads_at_start = 0;
    std::uint64_t unloads_at_start = 0;
    bool traversal_open = false;
    auto close_traversal = [&](bool complete) {
        if (!traversal_open) {
            return;
        }
        current.loads = manager.stats().loads - loads_at_start;
        current.unloads = manager.stats().unloads - unloads_at_start;
        current.complete = complete;
        m.traversals.push_back(current);
        traversal_open = false;
    };

    for (std::uint64_t frame = 0; frame < total_frames; ++frame) {
        const double travelled = speed_mps * static_cast<double>(frame) * kStepSeconds;
        const auto index = static_cast<std::uint64_t>(std::floor(travelled / cycle));
        if (!traversal_open || index != current.index) {
            close_traversal(true);
            current = Traversal{};
            current.index = index;
            loads_at_start = manager.stats().loads;
            unloads_at_start = manager.stats().unloads;
            traversal_open = true;
        }
        const glm::dvec2 camera = route.at(route_distance(travelled, cycle));

        const auto start = Clock::now();
        manager.update(camera);
        const auto after_update = Clock::now();
        manager.begin_frame();
        manager.end_frame();
        manager.safe_point();
        const auto end = Clock::now();

        const bool measured = frame >= warmup_frames;
        if (measured) {
            if (frame > warmup_frames) {
                m.frame_ms.push_back(ms_between(previous_start, start));
            }
            m.update_ms.push_back(ms_between(start, after_update));
            m.safe_point_ms.push_back(ms_between(after_update, end));
            const auto& stats = manager.stats();
            m.io_bytes += stats.io_bytes_last_update;
            m.decode_bytes += stats.decode_bytes_last_update;
            m.gpu_upload_bytes += stats.gpu_bytes_last_update;
            ++m.measured_frames;
        }
        previous_start = start;

        const auto& stats = manager.stats();
        current.peak_resident_cells = std::max<std::uint64_t>(current.peak_resident_cells, stats.resident_cells);
        current.peak_resident_cpu_bytes = std::max<std::uint64_t>(current.peak_resident_cpu_bytes, stats.resident_cpu_bytes);

        if (frame % kSampleEveryFrames == 0) {
            ++m.window_samples;
            const world::CellCoord camera_cell = world::cell_coord_at(catalog.world.desc, camera);
            for (const world::CellCoord& coord : window_cells(catalog, camera)) {
                const auto phase = manager.phase(coord);
                if (phase == world::CellPhase::Unloaded || phase == world::CellPhase::Failed) {
                    ++m.missing_window_cells;
                }
            }
            for (const world::CellCoord& coord : manager.active_cells()) {
                if (cell_distance(coord, camera_cell) > world::StreamingWindow{}.active_radius) {
                    ++m.stale_active_cells;
                }
            }
        }

        if (settings.paced) {
            const auto due = epoch + std::chrono::duration_cast<Clock::duration>(
                                         std::chrono::duration<double>(static_cast<double>(frame + 1) * kStepSeconds));
            std::this_thread::sleep_until(due);
        }
        ++m.frames;
    }

    const double travelled_end = speed_mps * static_cast<double>(total_frames) * kStepSeconds;
    close_traversal(travelled_end >= static_cast<double>(current.index + 1) * cycle);

    const auto& stats = manager.stats();
    m.cells_loaded = counts.loaded;
    m.cells_unloaded = stats.unloads;
    m.corrupt_loads = counts.corrupt;
    m.dangling_references = counts.dangling;
    m.failed_cells = stats.failed_cells;
    m.cancelled_requests = stats.cancelled_requests;
    m.activations = stats.activations;
    m.deactivations = stats.deactivations;
    m.teleports = stats.teleports;
    m.peak_resident_cells = stats.peak_resident_cells;
    m.peak_resident_cpu_bytes = stats.peak_resident_cpu_bytes;
    m.peak_gpu_bytes = stats.peak_gpu_bytes;
    m.peak_transient_bytes = stats.peak_transient_bytes;
    return m;
}

bool window_all_active(const world::StreamingManager& manager, const std::vector<world::CellCoord>& window) {
    return std::all_of(window.begin(), window.end(),
                       [&](const world::CellCoord& coord) { return manager.phase(coord) == world::CellPhase::Active; });
}

// The teleport case: settle at the route start, jump to a distant corner, and settle again. The new window must be exactly the active set.
TeleportResult drive_teleport(const Catalog& catalog, const Route& route) {
    TeleportResult result;
    Counts counts;
    core::JobSystem jobs(4);
    auto checker = std::make_shared<Checker>(catalog, counts);
    auto path = core::Path::from_utf8(catalog.directory.string());
    world::StreamingManager manager(jobs, path.value(), catalog.world);
    manager.subscribe(checker);

    // The destination lies 11 cells (2816 m) east of the route start, off the route. The manager counts a move as a teleport only when it
    // is more than twice the prefetch radius (10 cells), so this is the shortest jump of the kind the case needs to check.
    const glm::dvec2 start = route.at(0.0);
    const glm::dvec2 far = start + glm::dvec2(2816.0, 0.0);

    // Settle at the start first, so the jump has a populated window to leave behind.
    for (std::uint64_t round = 0; round < kTeleportRoundLimit; ++round) {
        manager.update(start);
        jobs.wait_idle();
        manager.safe_point();
        if (window_all_active(manager, window_cells(catalog, start))) {
            break;
        }
    }
    const std::uint64_t teleports_before = manager.stats().teleports;

    const auto jump = Clock::now();
    bool settled = false;
    for (std::uint64_t round = 0; round < kTeleportRoundLimit && !settled; ++round) {
        const auto a = Clock::now();
        manager.update(far);
        const auto b = Clock::now();
        jobs.wait_idle();
        const auto c = Clock::now();
        manager.safe_point();
        const auto d = Clock::now();
        result.worst_frame_ms = std::max(result.worst_frame_ms, ms_between(a, b) + ms_between(c, d));
        settled = window_all_active(manager, window_cells(catalog, far));
    }
    result.settle_ms = ms_between(jump, Clock::now());
    result.settled = settled;

    const std::vector<world::CellCoord> window = window_cells(catalog, far);
    result.window_cells = window.size();
    const std::vector<world::CellCoord> active = manager.active_cells();
    result.active_cells = active.size();
    const world::CellCoord far_cell = world::cell_coord_at(catalog.world.desc, far);
    for (const world::CellCoord& coord : active) {
        if (cell_distance(coord, far_cell) > world::StreamingWindow{}.active_radius) {
            ++result.stale_active_cells;
        }
    }
    for (const world::CellCoord& coord : window) {
        if (manager.phase(coord) != world::CellPhase::Active) {
            ++result.missing_cells;
        }
    }
    result.teleports = manager.stats().teleports - teleports_before;
    result.dangling_references = counts.dangling;
    result.corrupt_loads = counts.corrupt;
    result.failed_cells = manager.stats().failed_cells;
    result.peak_transient_bytes = manager.stats().peak_transient_bytes;
    return result;
}

std::uint64_t percentile_us(std::vector<double> values) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(values.size())));
    const std::size_t index = std::min(values.size() - 1, rank == 0 ? 0 : rank - 1);
    return static_cast<std::uint64_t>(std::llround(values[index] * 1000.0));
}

// Keeps the scratch directory's name unique per run.
fs::path scratch_directory() {
    return fs::temp_directory_path() / ("engine_streaming_" + core::Guid::generate().to_string());
}

}  // namespace

glm::dvec2 Route::at(double distance) const {
    double remaining = std::clamp(distance, 0.0, length_m);
    for (std::size_t i = 0; i + 1 < waypoints.size(); ++i) {
        const glm::dvec2 leg = waypoints[i + 1] - waypoints[i];
        const double leg_length = glm::length(leg);
        if (remaining <= leg_length || i + 2 == waypoints.size()) {
            return leg_length > 0.0 ? waypoints[i] + leg * std::min(remaining / leg_length, 1.0) : waypoints[i];
        }
        remaining -= leg_length;
    }
    return waypoints.empty() ? glm::dvec2(0.0) : waypoints.back();
}

double route_distance(double travelled, double length) noexcept {
    const double cycle = 2.0 * length;
    const double phase = std::fmod(std::max(travelled, 0.0), cycle);
    return phase <= length ? phase : cycle - phase;
}

core::Result<Route, std::string> streaming_route(const SceneDef& scene) {
    if (scene.route != "streaming_route_v1") {
        return core::failure(std::string("streaming_stress needs the route streaming_route_v1, not '") + scene.route + "'");
    }
    const auto length = scene.counts.find("route_length_m");
    if (length == scene.counts.end()) {
        return core::failure(std::string("streaming_stress: the workload has no route_length_m"));
    }
    // The shared world's centre, and three legs: 2400 m east-west, 1200 m north, then 1800 m west.
    const glm::dvec2 centre(kRouteCentreM, kRouteCentreM);
    Route route;
    route.waypoints = {centre + glm::dvec2(-1200.0, -600.0), centre + glm::dvec2(1200.0, -600.0), centre + glm::dvec2(1200.0, 600.0),
                       centre + glm::dvec2(-600.0, 600.0)};
    for (std::size_t i = 0; i + 1 < route.waypoints.size(); ++i) {
        route.length_m += glm::length(route.waypoints[i + 1] - route.waypoints[i]);
    }
    if (std::fabs(route.length_m - static_cast<double>(length->second)) > 1e-6) {
        return core::failure("streaming_route_v1 is " + std::to_string(route.length_m) + " m, but the table says " +
                             std::to_string(length->second) + " m");
    }
    return core::success(route);
}

core::Result<Result, std::string> run(const SceneDef& scene, const RunSettings& settings) {
    if (scene.builtin) {
        return core::failure(std::string("streaming_stress is not a builtin scene"));
    }
    if (settings.duration_s <= 0.0 || settings.warmup_s < 0.0) {
        return core::failure(std::string("streaming_stress needs a positive duration and a non-negative warmup"));
    }
    const auto route = streaming_route(scene);
    if (!route.ok()) {
        return core::failure(route.error());
    }
    const auto speed = scene.counts.find("speed_m_per_s");
    if (speed == scene.counts.end() || speed->second == 0) {
        return core::failure(std::string("streaming_stress: the workload needs a positive speed_m_per_s"));
    }

    const fs::path directory = scratch_directory();
    const ScratchGuard scratch(directory);
    auto catalog = build_catalog(scene, directory);
    if (!catalog.ok()) {
        return core::failure("streaming_stress: " + catalog.error());
    }

    Result result;
    result.instances = catalog.value().instances;
    result.cells = catalog.value().authored.size();
    result.meshes = catalog.value().meshes;
    result.catalog_problems = catalog.value().problems;
    result.simulated_s = settings.warmup_s + settings.duration_s;
    const auto speed_mps = static_cast<double>(speed->second);
    result.route = drive_route(catalog.value(), route.value(), speed_mps, settings);
    result.teleport = drive_teleport(catalog.value(), route.value());
    return core::success(std::move(result));
}

bool stability_known(const Result& result) {
    return std::any_of(result.route.traversals.begin(), result.route.traversals.end(),
                       [](const Traversal& t) { return t.index >= 2 && t.complete; });
}

std::vector<std::string> failures(const Result& result) {
    std::vector<std::string> reasons;
    const Measurement& route = result.route;
    if (!result.catalog_problems.empty()) {
        reasons.push_back("the authored world is invalid: " + result.catalog_problems.front());
    }
    if (route.failed_cells > 0) {
        reasons.push_back(std::to_string(route.failed_cells) + " cell(s) failed to read or parse");
    }
    if (route.corrupt_loads > 0) {
        reasons.push_back(std::to_string(route.corrupt_loads) + " loaded cell(s) differ from the authored object count");
    }
    if (route.dangling_references > 0) {
        reasons.push_back(std::to_string(route.dangling_references) + " dangling cross-cell reference(s)");
    }
    if (route.missing_window_cells > 0) {
        reasons.push_back(std::to_string(route.missing_window_cells) + " active-window cell(s) were missing (Unloaded or Failed)");
    }
    if (route.stale_active_cells > 0) {
        reasons.push_back(std::to_string(route.stale_active_cells) + " Active cell(s) outside the active window");
    }
    if (route.peak_gpu_bytes > kGpuBudgetBytes) {
        reasons.push_back("peak GPU residency exceeds the 12 GiB budget");
    }
    if (route.peak_resident_cpu_bytes + route.peak_transient_bytes > kHostBudgetBytes) {
        reasons.push_back("peak host residency exceeds the 16 GiB budget");
    }

    // Stability: a later traversal of the same route must not hold more than the first warm one. A leak shows as growth.
    const Traversal* baseline = nullptr;
    for (const Traversal& t : route.traversals) {
        if (t.index == 1 && t.complete) {
            baseline = &t;
        }
    }
    if (baseline != nullptr) {
        for (const Traversal& t : route.traversals) {
            if (t.index < 2 || !t.complete) {
                continue;
            }
            if (t.peak_resident_cells > baseline->peak_resident_cells) {
                reasons.push_back("resident cells grew in traversal " + std::to_string(t.index) + " (" +
                                  std::to_string(t.peak_resident_cells) + " > " + std::to_string(baseline->peak_resident_cells) + ")");
            }
            if (static_cast<double>(t.peak_resident_cpu_bytes) >
                kGrowthTolerance * static_cast<double>(baseline->peak_resident_cpu_bytes)) {
                reasons.push_back("resident CPU bytes grew in traversal " + std::to_string(t.index));
            }
        }
    }

    const TeleportResult& tp = result.teleport;
    if (!tp.settled) {
        reasons.push_back("teleport did not settle on the new window");
    }
    if (tp.teleports != 1) {
        reasons.push_back("teleport case counted " + std::to_string(tp.teleports) + " teleports, expected 1");
    }
    if (tp.stale_active_cells > 0) {
        reasons.push_back("teleport left " + std::to_string(tp.stale_active_cells) + " Active cell(s) outside the new window");
    }
    if (tp.missing_cells > 0) {
        reasons.push_back("teleport left " + std::to_string(tp.missing_cells) + " window cell(s) not Active");
    }
    if (tp.dangling_references > 0 || tp.corrupt_loads > 0 || tp.failed_cells > 0) {
        reasons.push_back("teleport case has a dangling reference, a corrupt load, or a failed cell");
    }
    return reasons;
}

void run_streaming_stress(const SceneDef& scene, const RunSettings& settings, RunRecord& record) {
    const auto measured = run(scene, settings);
    if (!measured.ok()) {
        record.status = RunStatus::Incomplete;
        record.reason = measured.error();
        return;
    }
    const Result& result = measured.value();
    const Measurement& route = result.route;
    const TeleportResult& tp = result.teleport;

    record.frames_started = route.frames;
    record.frames.cpu_ms = route.frame_ms;
    record.counters = {
        {"descriptors", result.instances},
        {"authored_cells", result.cells},
        {"meshes", result.meshes},
        {"simulated_frames", route.frames},
        {"measured_frames", route.measured_frames},
        {"traversals_complete", static_cast<std::uint64_t>(std::count_if(route.traversals.begin(), route.traversals.end(),
                                                                         [](const Traversal& t) { return t.complete; }))},
        {"cells_loaded", route.cells_loaded},
        {"cells_unloaded", route.cells_unloaded},
        {"activations", route.activations},
        {"deactivations", route.deactivations},
        {"cancelled_requests", route.cancelled_requests},
        {"failed_cells", route.failed_cells},
        {"corrupt_loads", route.corrupt_loads},
        {"dangling_references", route.dangling_references},
        {"missing_window_cells", route.missing_window_cells},
        {"stale_active_cells", route.stale_active_cells},
        {"io_bytes", route.io_bytes},
        {"decode_bytes", route.decode_bytes},
        {"gpu_upload_bytes", route.gpu_upload_bytes},
        {"io_decode_upload_update_p99_us", percentile_us(route.update_ms)},
        {"safe_point_p99_us", percentile_us(route.safe_point_ms)},
        {"peak_resident_cells", route.peak_resident_cells},
        {"peak_resident_cpu_bytes", route.peak_resident_cpu_bytes},
        {"peak_gpu_bytes", route.peak_gpu_bytes},
        {"peak_transient_bytes", route.peak_transient_bytes},
        {"teleport_settled", tp.settled ? 1u : 0u},
        {"teleport_settle_ms", static_cast<std::uint64_t>(std::llround(tp.settle_ms))},
        {"teleport_worst_work_us", static_cast<std::uint64_t>(std::llround(tp.worst_frame_ms * 1000.0))},
        {"teleport_window_cells", tp.window_cells},
        {"teleport_active_cells", tp.active_cells},
        {"teleport_stale_active_cells", tp.stale_active_cells},
        {"teleport_missing_cells", tp.missing_cells},
    };
    for (const Traversal& t : route.traversals) {
        const std::string prefix = "traversal_" + std::to_string(t.index) + "_";
        record.counters[prefix + "peak_resident_cells"] = t.peak_resident_cells;
        record.counters[prefix + "peak_resident_cpu_bytes"] = t.peak_resident_cpu_bytes;
        record.counters[prefix + "loads"] = t.loads;
        record.counters[prefix + "unloads"] = t.unloads;
    }
    record.cpu = summarize(route.frame_ms);

    if (route.frame_ms.empty()) {
        record.status = RunStatus::Incomplete;
        record.reason = "no frames were measured";
        return;
    }
    const std::vector<std::string> reasons = failures(result);
    if (!reasons.empty()) {
        record.status = RunStatus::Failed;
        for (const std::string& reason : reasons) {
            record.reason += (record.reason.empty() ? "" : "; ") + reason;
        }
        return;
    }
    if (!stability_known(result)) {
        record.status = RunStatus::Incomplete;
        record.reason = "the run is too short to judge residency stability: it needs a complete third traversal of the route";
        return;
    }
    record.status = RunStatus::Passed;
    record.reason = "streaming diagnostic; no failed cell, missing window cell, dangling reference, or stale active cell; residency stable; teleport settled";
}

}  // namespace engine::bench::streaming
