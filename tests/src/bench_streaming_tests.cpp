// The streaming benchmark (US-078): the streaming_stress scene walks streaming_route_v1 across cell boundaries, reports the cell lifecycle
// and the stage work, runs a separate teleport case, and judges residency stability. The unit cases check the route, the traversal
// distance, and the judgement on constructed results. The integration case streams a reduced workload from real cell files.

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/streaming_stress.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using engine::bench::Manifest;
using engine::bench::SceneDef;
using engine::bench::streaming::Route;
using engine::bench::streaming::RunSettings;
using engine::bench::streaming::Traversal;
using engine::bench::streaming::route_distance;
using engine::bench::streaming::streaming_route;
namespace streaming = engine::bench::streaming;

Manifest repository_manifest() {
    const auto path = engine::core::Path::from_utf8(ENGINE_TEST_BENCH_MANIFEST);
    REQUIRE(path.ok());
    const auto text = engine::core::read_text_file(path.value());
    REQUIRE(text.ok());
    auto manifest = engine::bench::parse_manifest(text.value());
    REQUIRE(manifest.ok());
    return std::move(manifest).value();
}

// A traversal record with the residency peaks the judgement reads.
Traversal traversal(std::uint64_t index, std::uint64_t cells, std::uint64_t bytes, bool complete = true) {
    Traversal t;
    t.index = index;
    t.peak_resident_cells = cells;
    t.peak_resident_cpu_bytes = bytes;
    t.complete = complete;
    return t;
}

// A result that passes every check, to be changed one field at a time.
streaming::Result clean_result() {
    streaming::Result result;
    result.route.frame_ms = {16.7};
    result.route.frames = 1;
    result.route.measured_frames = 1;
    result.teleport.settled = true;
    result.teleport.teleports = 1;
    result.teleport.window_cells = 81;
    result.teleport.active_cells = 81;
    return result;
}

}  // namespace

TEST_CASE("Streaming route: streaming_route_v1 is 5400 m, starts and ends at its waypoints", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef& scene = manifest.scenes.at("streaming_stress");
    const auto route = streaming_route(scene);
    REQUIRE(route.ok());
    CHECK(std::fabs(route.value().length_m - 5400.0) < 1e-6);
    REQUIRE(route.value().waypoints.size() == 4);
    const glm::dvec2 start = route.value().at(0.0);
    const glm::dvec2 end = route.value().at(route.value().length_m);
    CHECK(start == route.value().waypoints.front());
    CHECK(end == route.value().waypoints.back());
    // Past the end the camera stays at the end, and before the start at the start.
    CHECK(route.value().at(9000.0) == end);
    CHECK(route.value().at(-5.0) == start);
}

TEST_CASE("Streaming route: the camera walks the first leg and turns at the corner", "[unit]") {
    const Route route = [] {
        const Manifest manifest = repository_manifest();
        return streaming_route(manifest.scenes.at("streaming_stress")).value();
    }();
    // The first leg is 2400 m east; halfway along it is 1200 m from the start.
    const glm::dvec2 halfway = route.at(1200.0);
    CHECK(std::fabs(halfway.x - (route.waypoints[0].x + 1200.0)) < 1e-9);
    CHECK(std::fabs(halfway.y - route.waypoints[0].y) < 1e-9);
    // The corner at 2400 m turns north, so 2400 + 600 m puts the camera 600 m up the second leg.
    const glm::dvec2 up = route.at(3000.0);
    CHECK(std::fabs(up.x - route.waypoints[1].x) < 1e-9);
    CHECK(std::fabs(up.y - (route.waypoints[1].y + 600.0)) < 1e-9);
}

TEST_CASE("Streaming route: the traversal distance goes forward, then back, then forward", "[unit]") {
    const double length = 5400.0;
    CHECK(route_distance(0.0, length) == 0.0);
    CHECK(route_distance(length, length) == length);           // the far end
    CHECK(std::fabs(route_distance(1.5 * length, length) - 0.5 * length) < 1e-9);  // backtracking
    CHECK(std::fabs(route_distance(2.0 * length, length)) < 1e-9);  // back at the start
    CHECK(std::fabs(route_distance(2.25 * length, length) - 0.25 * length) < 1e-9);  // forward again
    CHECK(route_distance(-3.0, length) == 0.0);
}

TEST_CASE("Streaming judgement: a clean result passes and a stalled stream fails", "[unit]") {
    CHECK(streaming::failures(clean_result()).empty());

    auto failed_cell = clean_result();
    failed_cell.route.failed_cells = 1;
    CHECK(!streaming::failures(failed_cell).empty());

    auto dangling = clean_result();
    dangling.route.dangling_references = 2;
    CHECK(!streaming::failures(dangling).empty());

    auto missing = clean_result();
    missing.route.missing_window_cells = 3;
    CHECK(!streaming::failures(missing).empty());

    auto stale = clean_result();
    stale.route.stale_active_cells = 1;
    CHECK(!streaming::failures(stale).empty());

    auto unsettled = clean_result();
    unsettled.teleport.settled = false;
    CHECK(!streaming::failures(unsettled).empty());

    auto budget = clean_result();
    budget.route.peak_gpu_bytes = 13ull * 1024ull * 1024ull * 1024ull;
    CHECK(!streaming::failures(budget).empty());
}

TEST_CASE("Streaming judgement: residency that grows across traversals is a leak", "[unit]") {
    auto steady = clean_result();
    steady.route.traversals = {traversal(0, 60, 100), traversal(1, 70, 1000), traversal(2, 70, 1000)};
    CHECK(streaming::stability_known(steady));
    CHECK(streaming::failures(steady).empty());

    auto growing = clean_result();
    growing.route.traversals = {traversal(0, 60, 100), traversal(1, 70, 1000), traversal(2, 80, 1000)};
    CHECK(!streaming::failures(growing).empty());

    auto bytes = clean_result();
    bytes.route.traversals = {traversal(0, 60, 100), traversal(1, 70, 1000), traversal(2, 70, 1100)};
    CHECK(!streaming::failures(bytes).empty());

    // Two traversals are not enough to see a trend: the run is incomplete, not failed.
    auto short_run = clean_result();
    short_run.route.traversals = {traversal(0, 60, 100), traversal(1, 70, 1000)};
    CHECK(!streaming::stability_known(short_run));
    CHECK(streaming::failures(short_run).empty());
}

TEST_CASE("Streaming run: a reduced workload streams the route and settles the teleport", "[integration]") {
    Manifest manifest = repository_manifest();
    SceneDef scene = manifest.scenes.at("streaming_stress");
    scene.counts["descriptors"] = 4000;  // a reduced descriptor count; the route and its speed are the table's

    RunSettings settings;
    settings.warmup_s = 0.0;
    settings.duration_s = 40.0;  // 2400 frames: well under one traversal, so only the checks that need no trend apply
    const auto measured = streaming::run(scene, settings);
    REQUIRE(measured.ok());
    const streaming::Result& result = measured.value();

    CHECK(result.instances == 4000);
    CHECK(result.catalog_problems.empty());
    CHECK(result.route.frames == 2400);
    CHECK(result.route.measured_frames == 2400);
    CHECK(result.route.cells_loaded > 0);
    CHECK(result.route.missing_window_cells == 0);
    CHECK(result.route.stale_active_cells == 0);
    CHECK(result.route.corrupt_loads == 0);
    CHECK(result.route.dangling_references == 0);
    CHECK(result.route.failed_cells == 0);
    CHECK(result.route.peak_resident_cells > 0);
    CHECK(result.route.io_bytes > 0);

    CHECK(result.teleport.settled);
    CHECK(result.teleport.teleports == 1);
    CHECK(result.teleport.missing_cells == 0);
    CHECK(result.teleport.stale_active_cells == 0);
    CHECK(result.teleport.active_cells == result.teleport.window_cells);
    CHECK(!streaming::stability_known(result));
    CHECK(streaming::failures(result).empty());
}
