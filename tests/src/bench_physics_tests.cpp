// The physics benchmark (US-077): the physics_stress scene builds the table's bodies, constraints, and characters from the seeded workload,
// follows its fixed action sequence, and reports active and sleeping bodies, steps, queries, and catch-up events. The failure checks
// catch lost timesteps, non-finite transforms, and bodies outside the reference bounds. These run on the CPU, with no GPU.

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/physics_stress.hpp"
#include "engine/benchmark/report.hpp"
#include "engine/benchmark/workload.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"
#include "engine/physics/physics_world.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using engine::bench::Manifest;
using engine::bench::RunRecord;
using engine::bench::RunStatus;
using engine::bench::SceneDef;
using engine::bench::generate_physics_spec;
using engine::bench::generate_workload;
using engine::bench::physics::failures;
using engine::bench::physics::Result;
using engine::bench::physics::RunSettings;
using engine::bench::physics::run;
using engine::bench::physics::run_physics_stress;

Manifest repository_manifest() {
    const auto path = engine::core::Path::from_utf8(ENGINE_TEST_BENCH_MANIFEST);
    REQUIRE(path.ok());
    const auto text = engine::core::read_text_file(path.value());
    REQUIRE(text.ok());
    auto manifest = engine::bench::parse_manifest(text.value());
    REQUIRE(manifest.ok());
    return std::move(manifest).value();
}

// A short run of the table's scene: a quarter second of warmup and a second of measurement at the fixed step.
RunSettings short_settings() {
    RunSettings settings;
    settings.warmup_s = 0.25;
    settings.duration_s = 1.0;
    return settings;
}

}  // namespace

TEST_CASE("Physics benchmark: the scene builds the table's bodies, constraints, and characters", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef& scene = manifest.scenes.at("physics_stress");

    const auto spec = generate_physics_spec("physics_stress", scene);
    REQUIRE(spec.ok());
    CHECK(spec.value().bodies.size() == 2000);
    CHECK(spec.value().statics.size() == 200);
    CHECK(spec.value().constraints.size() == 100);
    CHECK(spec.value().characters.size() == 20);

    // The spec is the scene the digest covers, so its counts are the workload's counts.
    const auto workload = generate_workload("physics_stress", scene);
    REQUIRE(workload.ok());
    CHECK(workload.value().counts.at("bodies") == spec.value().bodies.size());
    CHECK(workload.value().counts.at("static_colliders") == spec.value().statics.size());
    CHECK(workload.value().counts.at("constraints") == spec.value().constraints.size());
    CHECK(workload.value().counts.at("physics_characters") == spec.value().characters.size());

    // Every joint names two different bodies, the same rule the generator keeps.
    for (const auto& link : spec.value().constraints) {
        CHECK(link.body_a != link.body_b);
        CHECK(link.body_a < spec.value().bodies.size());
        CHECK(link.body_b < spec.value().bodies.size());
    }
}

TEST_CASE("Physics benchmark: a builtin scene has no physics workload", "[unit]") {
    const Manifest manifest = repository_manifest();
    const auto spec = generate_physics_spec("calibration", manifest.scenes.at("calibration"));
    CHECK_FALSE(spec.ok());
}

TEST_CASE("Physics benchmark: the scene is created in the world and follows the fixed action sequence", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef& scene = manifest.scenes.at("physics_stress");

    auto measured = run(scene, short_settings());
    REQUIRE(measured.ok());
    const Result& result = measured.value();

    CHECK(result.created.dynamic_bodies == 2000);
    CHECK(result.created.static_colliders == 200);
    CHECK(result.created.ground == 1);
    CHECK(result.created.constraints == 100);
    CHECK(result.created.characters == 20);

    // A quarter second of warmup and a second of measurement: 75 frames, each one fixed step.
    CHECK(result.frames == 75);
    CHECK(result.measured_frames == 60);
    CHECK(result.frame_ms.size() == 60);
    CHECK(result.simulation_steps == 75);
    CHECK(result.catch_up_events == 0);
    CHECK(result.dropped_steps == 0);
    CHECK(result.update_errors == 0);

    // The queries follow their rates: 1000 raycasts and 100 shape casts a second, issued at fixed steps.
    CHECK(result.raycasts == 75 * 1000 / 60);
    CHECK(result.shape_casts == 75 * 100 / 60);
    CHECK(result.raycast_hits <= result.raycasts);
    CHECK(result.shape_cast_hits <= result.shape_casts);

    // Every body is kicked on its staggered schedule, so interactions happen and bodies are awake.
    CHECK(result.interactions > 0);
    CHECK(result.active_max > 0);
    CHECK(result.active_max <= 2000);
    // Sleeping is the dynamic count minus the awake count, so the least awake frame has the most sleepers.
    CHECK(result.active_min + result.sleeping_max == 2000);

    CHECK(result.nan_transforms == 0);
    CHECK(result.out_of_bounds_samples == 0);
    CHECK(failures(result).empty());
}

TEST_CASE("Physics benchmark: a frame the catch-up limit cannot absorb is a lost timestep", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef& scene = manifest.scenes.at("physics_stress");

    // Frame 5 stalls for a full second, which is 60 fixed steps. The catch-up limit runs four of them and drops the rest.
    RunSettings settings;
    settings.warmup_s = 0.0;
    settings.duration_s = 0.25;
    settings.frame_seconds = [](std::uint64_t frame) { return frame == 5 ? 1.0 : engine::physics::kFixedStep; };

    auto measured = run(scene, settings);
    REQUIRE(measured.ok());
    const Result& result = measured.value();
    CHECK(result.catch_up_events == 1);
    CHECK(result.dropped_steps > 0);

    const std::vector<std::string> reasons = failures(result);
    REQUIRE(reasons.size() == 1);
    CHECK(reasons.front().find("lost timesteps") != std::string::npos);
}

TEST_CASE("Physics benchmark: a non-finite transform and a body outside the bounds are failures", "[unit]") {
    Result clean;
    clean.frames = 60;
    CHECK(failures(clean).empty());

    Result nan;
    nan.nan_transforms = 3;
    const std::vector<std::string> nan_reasons = failures(nan);
    REQUIRE(nan_reasons.size() == 1);
    CHECK(nan_reasons.front().find("non-finite") != std::string::npos);

    Result outside;
    outside.out_of_bounds_samples = 2;
    const std::vector<std::string> outside_reasons = failures(outside);
    REQUIRE(outside_reasons.size() == 1);
    CHECK(outside_reasons.front().find("reference bounds") != std::string::npos);

    Result stepped;
    stepped.update_errors = 1;
    CHECK(failures(stepped).size() == 1);
}

TEST_CASE("Physics benchmark: the report carries the counts and the verdict", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef& scene = manifest.scenes.at("physics_stress");

    RunRecord record;
    record.scene = "physics_stress";
    record.profile = "physics_headless";
    run_physics_stress(scene, short_settings(), record);

    CHECK(record.status == RunStatus::Passed);
    CHECK(record.counters.at("bodies_dynamic") == 2000);
    CHECK(record.counters.at("constraints") == 100);
    CHECK(record.counters.at("characters") == 20);
    CHECK(record.counters.at("simulation_steps") == 75);
    CHECK(record.counters.at("measured_frames") == 60);
    CHECK(record.counters.at("lost_timesteps") == 0);
    CHECK(record.counters.count("active_bodies_mean") == 1);
    CHECK(record.counters.count("sleeping_bodies_max") == 1);
    CHECK(record.counters.count("raycasts") == 1);
    CHECK(record.counters.count("shape_casts") == 1);
    CHECK(record.frames.cpu_ms.size() == 60);
    CHECK(record.cpu.has_value());
}

TEST_CASE("Physics benchmark: a lost timestep fails the report", "[unit]") {
    const Manifest manifest = repository_manifest();
    const SceneDef& scene = manifest.scenes.at("physics_stress");

    RunRecord record;
    record.scene = "physics_stress";
    record.profile = "physics_headless";
    RunSettings settings;
    settings.duration_s = 0.25;
    settings.frame_seconds = [](std::uint64_t frame) { return frame == 2 ? 1.0 : engine::physics::kFixedStep; };
    run_physics_stress(scene, settings, record);

    CHECK(record.status == RunStatus::Failed);
    CHECK(record.reason.find("lost timesteps") != std::string::npos);
}
