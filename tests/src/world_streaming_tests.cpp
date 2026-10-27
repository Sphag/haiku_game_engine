// Asynchronous world streaming (US-049), on the CPU. These cases check the cell lifecycle as subscribers see it, activation only at
// safe points, a render consumer whose draw list changes only there, rapid direction changes, teleports, missing cells, the per-stage
// budgets, and tearing the manager down while requests are in flight. The fixture writes a real world to disk, so the jobs read files.

#include "engine/world/streaming.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/jobs.hpp"
#include "engine/core/path.hpp"
#include "engine/world/partition.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using engine::core::Guid;
using engine::core::JobSystem;
using engine::core::Path;
using engine::world::CellCoord;
using engine::world::CellData;
using engine::world::CellPhase;
using engine::world::CellSubscriber;
using engine::world::StreamingBudget;
using engine::world::StreamingManager;
using engine::world::StreamingWindow;
using engine::world::World;
using engine::world::WorldCell;
using engine::world::WorldDesc;
using engine::world::WorldObject;
namespace fs = std::filesystem;

namespace {

constexpr std::int32_t kRadius = 8;           // the authored world spans cells -8..8 on each axis
constexpr std::size_t kActiveCells = 81;      // a 9x9 active window, all of it authored
constexpr std::size_t kRingCells = 40;        // the one-cell prefetch ring around it: 11x11 minus 9x9
constexpr std::size_t kObjectsPerCell = 2;

// A point inside the cell at (x, z), away from its boundaries.
glm::vec2 at_cell(std::int32_t x, std::int32_t z) {
    return {static_cast<float>(x) * 256.0f + 128.0f, static_cast<float>(z) * 256.0f + 128.0f};
}

fs::path scratch_dir() {
    return fs::temp_directory_path() / ("engine_stream_" + Guid::generate().to_string());
}

Path path_of(const fs::path& directory) {
    auto path = Path::from_utf8(directory.string());
    return std::move(path.value());
}

// Removes the scratch directory when the test ends, even if a check failed part way through.
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

// Writes a (2 * kRadius + 1)^2 grid of cells, each with two objects, into `directory`.
World author_world(const fs::path& directory) {
    WorldDesc desc;
    desc.cell_size = 256.0f;
    auto created = engine::world::create_world(desc);
    REQUIRE(created.ok());
    World world = std::move(created.value());
    const Guid asset = Guid::generate();
    for (std::int32_t z = -kRadius; z <= kRadius; ++z) {
        for (std::int32_t x = -kRadius; x <= kRadius; ++x) {
            for (std::size_t n = 0; n < kObjectsPerCell; ++n) {
                WorldObject object;
                object.id = Guid::generate();
                object.name = "prop";
                object.asset = asset;
                object.position = glm::vec3(at_cell(x, z).x + static_cast<float>(n), 0.0f, at_cell(x, z).y);
                REQUIRE(engine::world::add_object(world, object, false).ok());
            }
        }
    }
    auto saved = engine::world::save_dirty(world, path_of(directory));
    REQUIRE(saved.ok());
    return world;
}

// Runs the manager until nothing is in flight: each round is one update, one wait for the jobs, and one safe point.
void settle(StreamingManager& manager, JobSystem& jobs, glm::vec2 camera, int rounds = 12) {
    for (int round = 0; round < rounds; ++round) {
        manager.update(glm::dvec2(camera));
        jobs.wait_idle();
        manager.safe_point();
    }
}

std::size_t count_phase(const StreamingManager& manager, CellPhase phase) {
    std::size_t count = 0;
    for (std::int32_t z = -kRadius; z <= kRadius; ++z) {
        for (std::int32_t x = -kRadius; x <= kRadius; ++x) {
            count += manager.phase(CellCoord{x, z}) == phase ? 1u : 0u;
        }
    }
    return count;
}

// Records every lifecycle event by cell. It also keeps the decoded data it was given, so a test can read it after unload.
class Recorder : public CellSubscriber {
public:
    void on_loaded(const CellData& cell) override {
        record("loaded", cell);
        held.push_back(cell);
    }
    void on_uploaded(const CellData& cell) override {
        record("uploaded", cell);
    }
    void on_activated(const CellData& cell) override {
        record("activated", cell);
    }
    void on_deactivated(const CellData& cell) override {
        record("deactivated", cell);
    }
    void on_unloaded(const CellData& cell) override {
        record("unloaded", cell);
    }

    std::size_t count(const std::string& kind) const {
        std::size_t total = 0;
        for (const auto& event : events) {
            total += event.first == kind ? 1u : 0u;
        }
        return total;
    }

    // The lifecycle steps of one cell, in the order they arrived.
    std::vector<std::string> steps_of(CellCoord coord) const {
        std::vector<std::string> steps;
        for (const auto& event : events) {
            if (event.second == coord) {
                steps.push_back(event.first);
            }
        }
        return steps;
    }

    std::vector<std::pair<std::string, CellCoord>> events;
    std::vector<CellData> held;

private:
    void record(const std::string& kind, const CellData& cell) {
        events.emplace_back(kind, cell->coord);
    }
};

// A render consumer: it keeps the objects of the active cells as its draw list, and it changes that list only in the subscriber calls.
class RenderConsumer : public CellSubscriber {
public:
    void on_activated(const CellData& cell) override {
        draws[{cell->coord.x, cell->coord.z}] = cell->objects.size();
    }
    void on_deactivated(const CellData& cell) override {
        draws.erase({cell->coord.x, cell->coord.z});
    }
    void on_unloaded(const CellData& cell) override {
        draws.erase({cell->coord.x, cell->coord.z});
    }

    std::size_t objects() const {
        std::size_t total = 0;
        for (const auto& [key, count] : draws) {
            total += count;
        }
        return total;
    }

    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> draws;
};

}  // namespace

TEST_CASE("Cells load in rings, and only the active window is uploaded and activated", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    StreamingManager manager(jobs, path_of(scratch.directory), world);

    settle(manager, jobs, at_cell(0, 0));

    CHECK(manager.phase(CellCoord{0, 0}) == CellPhase::Active);
    CHECK(manager.phase(CellCoord{4, -4}) == CellPhase::Active);
    CHECK(manager.active_cells().size() == kActiveCells);
    CHECK(manager.stats().active_cells == kActiveCells);
    CHECK(manager.phase(CellCoord{5, 0}) == CellPhase::Loaded);  // the ring is read and decoded, not uploaded
    CHECK(manager.phase(CellCoord{6, 0}) == CellPhase::Unloaded);
    CHECK(count_phase(manager, CellPhase::Loaded) == kRingCells);
    CHECK(manager.stats().resident_cells == kActiveCells + kRingCells);
    CHECK(manager.stats().resident_cpu_bytes > 0);
    CHECK(manager.stats().gpu_bytes > 0);
}

TEST_CASE("Each cell's lifecycle arrives in order, and activation waits for the safe point", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    auto recorder = std::make_shared<Recorder>();
    StreamingManager manager(jobs, path_of(scratch.directory), world);
    manager.subscribe(recorder);

    // Without a safe point, the reads, decodes, and uploads run but nothing is activated.
    for (int round = 0; round < 6; ++round) {
        manager.update(glm::dvec2(at_cell(0, 0)));
        jobs.wait_idle();
    }
    CHECK(recorder->count("loaded") > 0);
    CHECK(recorder->count("uploaded") > 0);
    CHECK(recorder->count("activated") == 0);

    manager.safe_point();
    CHECK(recorder->count("activated") == kActiveCells);

    const auto steps = recorder->steps_of(CellCoord{0, 0});
    REQUIRE(steps.size() == 3);
    CHECK(steps[0] == "loaded");
    CHECK(steps[1] == "uploaded");
    CHECK(steps[2] == "activated");
}

TEST_CASE("A render consumer's draw list changes only at safe points", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    auto consumer = std::make_shared<RenderConsumer>();
    StreamingManager manager(jobs, path_of(scratch.directory), world);
    manager.subscribe(consumer);
    settle(manager, jobs, at_cell(0, 0));
    CHECK(consumer->objects() == kActiveCells * kObjectsPerCell);

    // Move one cell east. The new cells load, but the draw list keeps its old contents until the safe point.
    manager.update(at_cell(1, 0));
    jobs.wait_idle();
    CHECK(consumer->objects() == kActiveCells * kObjectsPerCell);

    manager.begin_frame();
    const std::size_t drawn_this_frame = consumer->objects();
    manager.end_frame();
    CHECK(drawn_this_frame == kActiveCells * kObjectsPerCell);

    manager.safe_point();
    CHECK(consumer->objects() == manager.stats().active_cells * kObjectsPerCell);
    CHECK(manager.stats().active_cells == kActiveCells);
    CHECK(manager.phase(CellCoord{-4, 0}) == CellPhase::Ready);  // left the window: still resident, no longer drawn
    CHECK(consumer->draws.count({-4, 0}) == 0);
}

TEST_CASE("Rapid direction changes cancel stale requests and leave the window correct", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    StreamingManager manager(jobs, path_of(scratch.directory), world);

    // Alternate east and west across cell boundaries without waiting, so requests are started and abandoned while jobs run.
    for (int step = 0; step < 200; ++step) {
        const float sign = (step % 2 == 0) ? 1.0f : -1.0f;
        manager.update(at_cell(0, 0) + glm::vec2(sign * 300.0f * static_cast<float>(step % 5), sign * 40.0f));
        if (step % 7 == 0) {
            jobs.wait_idle();
            manager.safe_point();
        }
    }
    CHECK(manager.stats().cancelled_requests > 0);

    settle(manager, jobs, at_cell(0, 0), 20);
    CHECK(manager.active_cells().size() == kActiveCells);
    CHECK(manager.phase(CellCoord{0, 0}) == CellPhase::Active);
    CHECK(count_phase(manager, CellPhase::Reading) == 0);
    CHECK(count_phase(manager, CellPhase::Decoding) == 0);
}

TEST_CASE("A teleport unloads the old window and loads the new one, with bounded residency", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    auto recorder = std::make_shared<Recorder>();
    StreamingManager manager(jobs, path_of(scratch.directory), world);
    manager.subscribe(recorder);

    settle(manager, jobs, at_cell(-8, -8));
    CHECK(manager.phase(CellCoord{-8, -8}) == CellPhase::Active);
    const std::size_t resident_before = manager.stats().resident_cells;

    // (-8,-8) to (8,8) is 16 cells, more than the 10 that the two rings share, so the windows do not overlap.
    settle(manager, jobs, at_cell(8, 8));
    CHECK(manager.stats().teleports == 1);
    CHECK(manager.phase(CellCoord{-8, -8}) == CellPhase::Unloaded);
    CHECK(manager.phase(CellCoord{8, 8}) == CellPhase::Active);
    CHECK(manager.active_cells().size() == 25);  // x and z in 4..8, the part of the window the world covers
    CHECK(manager.stats().resident_cells == 36);  // x and z in 3..8, the window plus its ring
    CHECK(resident_before == 36);
    CHECK(manager.stats().unloads == 36);
    CHECK(manager.stats().peak_resident_cells <= 121);  // 11x11, the most a window and its ring can hold
    CHECK(manager.stats().peak_transient_bytes > 0);

    // A subscriber keeps the decoded cell it was given. It stays readable after the manager has unloaded it.
    REQUIRE(!recorder->held.empty());
    for (const auto& cell : recorder->held) {
        CHECK(cell->objects.size() == kObjectsPerCell);
    }
}

TEST_CASE("A missing cell fails once, does not block its neighbours, and is retried only after it leaves the ring", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    const WorldCell* missing = nullptr;
    for (const auto& cell : world.cells) {
        if (cell.coord == CellCoord{0, 0}) {
            missing = &cell;
        }
    }
    REQUIRE(missing != nullptr);
    std::error_code ec;
    REQUIRE(fs::remove(scratch.directory / engine::world::cell_file_name(*missing), ec));

    JobSystem jobs(2);
    auto recorder = std::make_shared<Recorder>();
    StreamingManager manager(jobs, path_of(scratch.directory), world);
    manager.subscribe(recorder);

    settle(manager, jobs, at_cell(0, 0));
    CHECK(manager.phase(CellCoord{0, 0}) == CellPhase::Failed);
    CHECK_FALSE(manager.error(CellCoord{0, 0}).empty());
    CHECK(manager.phase(CellCoord{1, 0}) == CellPhase::Active);
    CHECK(manager.stats().failed_cells == 1);
    CHECK(recorder->steps_of(CellCoord{0, 0}).empty());

    // A failed cell inside the ring is not retried every update.
    settle(manager, jobs, at_cell(0, 0), 5);
    CHECK(manager.stats().failed_cells == 1);

    // Leaving the ring forgets the failure, and coming back retries the read.
    settle(manager, jobs, at_cell(8, 8));
    CHECK(manager.phase(CellCoord{0, 0}) == CellPhase::Unloaded);
    CHECK(manager.error(CellCoord{0, 0}).empty());
    settle(manager, jobs, at_cell(0, 0));
    CHECK(manager.phase(CellCoord{0, 0}) == CellPhase::Failed);
    CHECK(manager.stats().failed_cells == 2);
}

TEST_CASE("Each stage's budget limits the work per update, and a stage always starts one item", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    StreamingBudget tight;
    tight.io_bytes_per_update = 1;
    tight.decode_bytes_per_update = 1;
    tight.gpu_upload_bytes_per_update = 1;
    StreamingManager manager(jobs, path_of(scratch.directory), world, StreamingWindow{}, tight);

    manager.update(glm::dvec2(at_cell(0, 0)));
    CHECK(count_phase(manager, CellPhase::Reading) == 1);
    jobs.wait_idle();

    // One upload per update at most. The per-update GPU spend stays at one cell's estimate.
    const std::size_t one_cell_gpu = 256 + 96 * kObjectsPerCell;
    std::size_t largest_upload = 0;
    for (int round = 0; round < 600; ++round) {
        manager.update(glm::dvec2(at_cell(0, 0)));
        jobs.wait_idle();
        manager.safe_point();
        largest_upload = std::max(largest_upload, manager.stats().gpu_bytes_last_update);
        if (manager.stats().active_cells == kActiveCells) {
            break;
        }
    }
    CHECK(largest_upload <= one_cell_gpu);
    CHECK(manager.stats().active_cells == kActiveCells);
}

TEST_CASE("Destroying the manager with requests in flight unloads resident cells and frees the requests", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    auto recorder = std::make_shared<Recorder>();
    {
        StreamingManager manager(jobs, path_of(scratch.directory), world);
        manager.subscribe(recorder);
        settle(manager, jobs, at_cell(0, 0));
        const std::size_t resident = manager.stats().resident_cells;
        CHECK(resident == kActiveCells + kRingCells);

        // Reads for a new area are in flight when the manager goes away.
        manager.update(at_cell(-8, -8));
        CHECK(manager.stats().resident_cells == resident);
        const std::size_t activated_before = recorder->count("activated");
        CHECK(activated_before == kActiveCells);
    }
    jobs.wait_idle();

    CHECK(recorder->count("deactivated") == kActiveCells);
    CHECK(recorder->count("unloaded") == kActiveCells + kRingCells);
}

TEST_CASE("A suppressed cell leaves even inside the active window, and a pinned cell stays outside the ring", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    StreamingManager manager(jobs, path_of(scratch.directory), world);
    settle(manager, jobs, at_cell(0, 0));
    REQUIRE(manager.phase(CellCoord{0, 0}) == CellPhase::Active);

    // Suppression drops the centre cell although the camera still covers it, and the window fills around the gap.
    manager.set_suppressed(CellCoord{0, 0}, true);
    CHECK(manager.suppressed(CellCoord{0, 0}));
    settle(manager, jobs, at_cell(0, 0));
    CHECK(manager.phase(CellCoord{0, 0}) == CellPhase::Unloaded);
    CHECK(manager.stats().active_cells == kActiveCells - 1);

    // A pin keeps a cell that is one past the prefetch ring resident, though it is never activated there.
    manager.set_suppressed(CellCoord{0, 0}, false);
    manager.set_pinned(CellCoord{7, 0}, true);
    CHECK(manager.pinned(CellCoord{7, 0}));
    settle(manager, jobs, at_cell(0, 0));
    CHECK(manager.phase(CellCoord{7, 0}) == CellPhase::Loaded);

    manager.set_pinned(CellCoord{7, 0}, false);
    settle(manager, jobs, at_cell(0, 0));
    CHECK(manager.phase(CellCoord{7, 0}) == CellPhase::Unloaded);
}

TEST_CASE("Suppression wins over a pin, and clearing it lets the cell return", "[unit]") {
    const ScratchGuard scratch(scratch_dir());
    const World world = author_world(scratch.directory);
    JobSystem jobs(2);
    StreamingManager manager(jobs, path_of(scratch.directory), world);
    manager.set_pinned(CellCoord{-7, -7}, true);
    manager.set_suppressed(CellCoord{-7, -7}, true);
    settle(manager, jobs, at_cell(0, 0));
    CHECK(manager.phase(CellCoord{-7, -7}) == CellPhase::Unloaded);

    manager.set_suppressed(CellCoord{-7, -7}, false);
    settle(manager, jobs, at_cell(0, 0));
    CHECK(manager.phase(CellCoord{-7, -7}) == CellPhase::Loaded);
}
