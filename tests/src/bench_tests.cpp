// Benchmark harness (US-074): the manifest contract, the percentile and threshold rules of the performance contract, the
// exit-code rules, the command line, and the report files. These checks use in-memory values and a temporary folder. The
// repository manifest is parsed too, so an edit that breaks the contract fails here.

#include "engine/benchmark/cli.hpp"
#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/measure.hpp"
#include "engine/benchmark/report.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::bench::Check;
using engine::bench::RepeatEvidence;
using engine::bench::RunRecord;
using engine::bench::RunStatus;
using engine::bench::Verdict;

constexpr std::uint64_t kGiB = 1ull << 30;

RepeatEvidence steady_evidence(std::size_t frames, double cpu_ms, double gpu_ms) {
    RepeatEvidence evidence;
    evidence.frames.cpu_ms.assign(frames, cpu_ms);
    evidence.frames.gpu_ms.assign(frames, gpu_ms);
    evidence.frames.gpu_present = true;
    evidence.gpu_resident_bytes = 4 * kGiB;
    evidence.host_working_set_bytes = 2 * kGiB;
    return evidence;
}

std::string read_repository_manifest() {
    const auto path = engine::core::Path::from_utf8(ENGINE_TEST_BENCH_MANIFEST);
    REQUIRE(path.ok());
    const auto text = engine::core::read_text_file(path.value());
    REQUIRE(text.ok());
    return text.value();
}

// A minimal manifest with one acceptance-shaped suite. The edits below break one rule at a time.
std::string small_manifest(std::string_view suite_timing = R"({"warmup_s": 30, "duration_s": 180, "repeats": 3})",
                           std::string_view feature = "RayTracing") {
    return std::string(R"({
  "manifest_version": 1,
  "world": {},
  "profiles": {
    "p": {"description": "d", "frame_contract": true, "requires_features": [")") +
           std::string(feature) + R"("], "settings": {}}
  },
  "scenes": {
    "s": {"implementation": "none", "seed": 3, "workload": {"n": 4}, "route": null}
  },
  "suites": {
    "acceptance": {"description": "d", "timing": )" + std::string(suite_timing) +
           R"(, "runs": [{"scene": "s", "profile": "p"}]}
  }
})";
}

RunRecord run_with(RunStatus status) {
    RunRecord run;
    run.status = status;
    return run;
}

engine::bench::Report report_with(std::vector<RunStatus> statuses) {
    engine::bench::Report report;
    report.run_id = "test";
    for (const RunStatus status : statuses) {
        report.runs.push_back(run_with(status));
    }
    return report;
}

}  // namespace

TEST_CASE("Bench: nearest-rank percentiles match ceil(p*n) on a known set", "[unit]") {
    std::vector<double> values;
    for (int i = 100; i >= 1; --i) {
        values.push_back(static_cast<double>(i));
    }
    const auto summary = engine::bench::summarize(values);
    CHECK(summary.count == 100);
    CHECK(summary.p50 == 50.0);
    CHECK(summary.p95 == 95.0);
    CHECK(summary.p99 == 99.0);
    CHECK(summary.max == 100.0);
    CHECK(summary.mean == 50.5);

    const auto empty = engine::bench::summarize({});
    CHECK(empty.count == 0);
}

TEST_CASE("Bench: a steady run within the contract passes every check", "[unit]") {
    const auto result = engine::bench::evaluate_contract(steady_evidence(1000, 10.0, 8.0));
    CHECK(result.verdict == Verdict::Passed);
    CHECK(result.checks.size() == 6);
}

TEST_CASE("Bench: p99 above 1000/60 ms fails the repeat", "[unit]") {
    auto evidence = steady_evidence(100, 10.0, 8.0);
    for (std::size_t i = 0; i < 3; ++i) {
        evidence.frames.cpu_ms[i] = 20.0;
    }
    const auto result = engine::bench::evaluate_contract(evidence);
    CHECK(result.verdict == Verdict::Failed);
}

TEST_CASE("Bench: frames over 1000/30 ms beyond 0.1% fail, and at or below it pass", "[unit]") {
    auto over = steady_evidence(1000, 10.0, 8.0);
    over.frames.cpu_ms[0] = 40.0;
    over.frames.cpu_ms[1] = 40.0;
    CHECK(engine::bench::evaluate_contract(over).verdict == Verdict::Failed);

    auto within = steady_evidence(2000, 10.0, 8.0);
    within.frames.cpu_ms[0] = 40.0;
    CHECK(engine::bench::evaluate_contract(within).verdict == Verdict::Passed);
}

TEST_CASE("Bench: a single frame over 100 ms fails even when the percentiles pass", "[unit]") {
    auto evidence = steady_evidence(1000, 10.0, 8.0);
    evidence.frames.cpu_ms[500] = 150.0;
    const auto result = engine::bench::evaluate_contract(evidence);
    CHECK(result.verdict == Verdict::Failed);
    bool max_check_failed = false;
    for (const Check& check : result.checks) {
        max_check_failed = max_check_failed || (check.name == "no frame over 100 ms" && !check.passed);
    }
    CHECK(max_check_failed);
}

TEST_CASE("Bench: acceptance frame time is max(cpu, gpu) per frame", "[unit]") {
    // CPU is fine, but the GPU span is 20 ms every frame, so the contract's frame time exceeds the p99 budget.
    const auto result = engine::bench::evaluate_contract(steady_evidence(200, 10.0, 20.0));
    CHECK(result.verdict == Verdict::Failed);
}

TEST_CASE("Bench: missing GPU timings, missing residency, or no frames are incomplete, never passed", "[unit]") {
    auto no_gpu = steady_evidence(200, 10.0, 8.0);
    no_gpu.frames.gpu_present = false;
    no_gpu.frames.gpu_ms.clear();
    CHECK(engine::bench::evaluate_contract(no_gpu).verdict == Verdict::Incomplete);

    auto no_residency = steady_evidence(200, 10.0, 8.0);
    no_residency.gpu_resident_bytes.reset();
    CHECK(engine::bench::evaluate_contract(no_residency).verdict == Verdict::Incomplete);

    RepeatEvidence none;
    CHECK(engine::bench::evaluate_contract(none).verdict == Verdict::Incomplete);
}

TEST_CASE("Bench: residency above 12 GiB GPU or 16 GiB host fails the repeat", "[unit]") {
    auto gpu_over = steady_evidence(200, 10.0, 8.0);
    gpu_over.gpu_resident_bytes = 13 * kGiB;
    CHECK(engine::bench::evaluate_contract(gpu_over).verdict == Verdict::Failed);

    auto host_over = steady_evidence(200, 10.0, 8.0);
    host_over.host_working_set_bytes = 17 * kGiB;
    CHECK(engine::bench::evaluate_contract(host_over).verdict == Verdict::Failed);
}

TEST_CASE("Bench: a timing failure is not hidden when residency is also missing", "[unit]") {
    auto evidence = steady_evidence(1000, 10.0, 8.0);
    evidence.frames.cpu_ms[0] = 150.0;
    evidence.gpu_resident_bytes.reset();
    CHECK(engine::bench::evaluate_contract(evidence).verdict == Verdict::Failed);
}

TEST_CASE("Bench: the calibration workload is deterministic for a seed and differs across seeds", "[unit]") {
    engine::bench::CalibrationWorkload a(42, 256, 4);
    engine::bench::CalibrationWorkload b(42, 256, 4);
    engine::bench::CalibrationWorkload c(43, 256, 4);
    for (int i = 0; i < 3; ++i) {
        a.frame();
        b.frame();
        c.frame();
    }
    CHECK(a.checksum() == b.checksum());
    CHECK(a.checksum() != c.checksum());
    CHECK(a.checksum() != 0);
}

TEST_CASE("Bench: run_frames times only frames that start after the warmup", "[unit]") {
    std::uint64_t started = 0;
    const auto intervals = engine::bench::run_frames(0.02, 0.05, [] {}, started);
    CHECK(started >= 3);
    CHECK(!intervals.empty());
    CHECK(intervals.size() < started);
    for (const double ms : intervals) {
        CHECK(ms >= 0.0);
    }
}

TEST_CASE("Bench: the repository manifest parses and names the contract workloads", "[unit]") {
    const auto manifest = engine::bench::parse_manifest(read_repository_manifest());
    REQUIRE(manifest.ok());
    const auto& m = manifest.value();
    CHECK(m.sha256.size() == 64);
    REQUIRE(m.suites.count("acceptance") == 1);
    CHECK(m.suites.at("acceptance").runs.size() == 8);
    CHECK(m.suites.at("acceptance").timing.warmup_s == 30.0);
    CHECK(m.suites.at("acceptance").timing.duration_s == 180.0);
    CHECK(m.suites.at("acceptance").timing.repeats == 3);
    REQUIRE(m.suites.count("smoke") == 1);
    CHECK(m.scenes.at("calibration").builtin);
    CHECK(!m.scenes.at("render_stress").builtin);
    CHECK(m.scenes.at("render_stress").counts.at("renderable_instances") == 20000);
    CHECK(m.profiles.at("full_1080").requires_features.size() == 1);
    CHECK(!m.profiles.at("physics_headless").frame_contract);
    CHECK(m.profiles.at("physics_visual").frame_contract);
}

TEST_CASE("Bench: a manifest that breaks a rule is refused with the reason", "[unit]") {
    CHECK(engine::bench::parse_manifest(small_manifest()).ok());

    const auto version = engine::bench::parse_manifest(R"({"manifest_version": 2})");
    CHECK(!version.ok());

    const auto timing = engine::bench::parse_manifest(small_manifest(R"({"warmup_s": 1, "duration_s": 180, "repeats": 3})"));
    REQUIRE(!timing.ok());
    CHECK(timing.error().find("fixed by the performance contract") != std::string::npos);

    const auto feature = engine::bench::parse_manifest(small_manifest(R"({"warmup_s": 30, "duration_s": 180, "repeats": 3})", "RayTrcing"));
    REQUIRE(!feature.ok());
    CHECK(feature.error().find("unknown feature") != std::string::npos);

    std::string unknown_scene = small_manifest();
    unknown_scene.replace(unknown_scene.find("\"scene\": \"s\""), 12, "\"scene\": \"x\"");
    const auto scene = engine::bench::parse_manifest(unknown_scene);
    REQUIRE(!scene.ok());
    CHECK(scene.error().find("unknown scene") != std::string::npos);

    const auto parse = engine::bench::parse_manifest("{ not json");
    CHECK(!parse.ok());
}

TEST_CASE("Bench: the command line accepts the two suites and refuses misuse with a reason", "[unit]") {
    using engine::bench::parse_command_line;
    const auto smoke = parse_command_line({"--suite", "smoke", "--output", "out"});
    CHECK(smoke.error.empty());
    CHECK(smoke.options.suite == "smoke");

    const auto overridden = parse_command_line({"--suite", "smoke", "--output", "out", "--warmup", "0", "--duration", "0.5", "--repeats", "2"});
    CHECK(overridden.error.empty());
    CHECK(overridden.options.repeats.value() == 2);

    const auto acceptance = parse_command_line({"--suite", "acceptance", "--backend", "vulkan", "--output", "out"});
    CHECK(acceptance.error.empty());

    CHECK(parse_command_line({"--suite", "acceptance", "--output", "out"}).error.find("requires --backend") != std::string::npos);
    CHECK(parse_command_line({"--suite", "smoke", "--backend", "dx12", "--output", "out"}).error.find("takes no --backend") !=
          std::string::npos);
    CHECK(parse_command_line({"--suite", "acceptance", "--backend", "vulkan", "--output", "out", "--repeats", "1"}).error.find("smoke only") !=
          std::string::npos);
    CHECK(parse_command_line({"--suite", "smoke", "--output", "out", "--repeats", "0"}).error.find("integer") != std::string::npos);
    CHECK(parse_command_line({"--suite", "smoke"}).error.find("--output") != std::string::npos);
    CHECK(parse_command_line({"--suite", "bogus", "--output", "out"}).error.find("invalid suite") != std::string::npos);
    CHECK(parse_command_line({"--bogus"}).error.find("unknown argument") != std::string::npos);
    CHECK(parse_command_line({"--suite"}).error.find("requires a value") != std::string::npos);
    CHECK(parse_command_line({"--help"}).options.show_help);
}

TEST_CASE("Bench: exit codes separate threshold failures from runs that were not verified", "[unit]") {
    CHECK(engine::bench::exit_code(report_with({RunStatus::Passed, RunStatus::Passed})) == 0);
    CHECK(engine::bench::exit_code(report_with({RunStatus::Passed, RunStatus::Failed})) == 1);
    CHECK(engine::bench::exit_code(report_with({RunStatus::Missing, RunStatus::Passed})) == 3);
    CHECK(engine::bench::exit_code(report_with({RunStatus::Unsupported})) == 3);
    CHECK(engine::bench::exit_code(report_with({RunStatus::Incomplete})) == 3);
    CHECK(engine::bench::exit_code(report_with({RunStatus::Missing, RunStatus::Failed})) == 1);
    CHECK(engine::bench::exit_code(report_with({})) == 3);
}

TEST_CASE("Bench: a report folder holds report.json and raw frame CSV files", "[unit]") {
    const auto dir = std::filesystem::temp_directory_path() / "engine_bench_unit_report";
    std::filesystem::remove_all(dir);

    auto report = report_with({RunStatus::Passed});
    report.suite = "smoke";
    report.backend = "none";
    report.manifest_sha256 = std::string(64, 'a');
    report.workloads["calibration"] = R"({"seed": 1})";
    report.profiles["builtin"] = "{}";
    report.runs[0].scene = "calibration";
    report.runs[0].profile = "builtin";
    report.runs[0].frames.cpu_ms = {1.5, 2.25};
    report.runs[0].raw_file = "raw/calibration-builtin-none-r1.csv";
    report.runs[0].counters["checksum"] = 7;

    const auto written = engine::bench::write_report(report, dir);
    REQUIRE(written.ok());
    CHECK(std::filesystem::exists(dir / "report.json"));

    std::string header;
    {
        // Scoped so the handle is closed before the folder is removed; Windows refuses to delete an open file.
        std::ifstream csv(dir / report.runs[0].raw_file);
        std::getline(csv, header);
    }
    CHECK(header == "frame,cpu_ms,gpu_ms");

    std::string text;
    {
        std::ifstream json(dir / "report.json");
        text.assign(std::istreambuf_iterator<char>(json), std::istreambuf_iterator<char>());
    }
    CHECK(text.find("\"schema\": \"engine_bench.report\"") != std::string::npos);
    CHECK(text.find("\"status\": \"passed\"") != std::string::npos);
    CHECK(text.find("\"exit_code\": 0") != std::string::npos);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

// The render route and the profile outputs (US-076) come from the manifest. A malformed one is refused, not defaulted.
TEST_CASE("Bench: the render route and the profile outputs parse, and a malformed one is refused", "[unit]") {
    const auto good = engine::bench::parse_manifest(read_repository_manifest());
    REQUIRE(good.ok());
    CHECK(good.value().profiles.at("native_1080").output_width == 1920);
    CHECK(good.value().profiles.at("native_1080").output_height == 1080);
    CHECK(good.value().scenes.at("render_stress").camera.has_value());
    CHECK(!good.value().scenes.at("all_features").camera.has_value());

    std::string bad_output = read_repository_manifest();
    const std::string output = "\"output\": \"1920x1080\"";
    REQUIRE(bad_output.find(output) != std::string::npos);
    bad_output.replace(bad_output.find(output), output.size(), "\"output\": \"-1920x1080\"");
    const auto output_result = engine::bench::parse_manifest(bad_output);
    REQUIRE(!output_result.ok());
    CHECK(output_result.error().find("WIDTHxHEIGHT") != std::string::npos);

    std::string bad_camera = read_repository_manifest();
    const std::string heading = "\"heading_deg\": 90";
    REQUIRE(bad_camera.find(heading) != std::string::npos);
    bad_camera.replace(bad_camera.find(heading), heading.size(), "\"heading_deg\": \"90\"");
    const auto camera_result = engine::bench::parse_manifest(bad_camera);
    REQUIRE(!camera_result.ok());
    CHECK(camera_result.error().find("heading_deg") != std::string::npos);
}

TEST_CASE("Bench: the command line takes a render path and the mesh-shader switch, and refuses an unknown path", "[unit]") {
    using engine::bench::parse_command_line;
    const auto parsed = parse_command_line({"--suite", "acceptance", "--backend", "dx12", "--output", "out", "--path", "meshlets",
                                            "--disable-mesh-shaders"});
    CHECK(parsed.error.empty());
    CHECK(parsed.options.path == "meshlets");
    CHECK(parsed.options.disable_mesh_shaders);

    const auto plain = parse_command_line({"--suite", "acceptance", "--backend", "dx12", "--output", "out"});
    CHECK(plain.options.path.empty());
    CHECK(!plain.options.disable_mesh_shaders);

    CHECK(parse_command_line({"--suite", "acceptance", "--backend", "dx12", "--output", "out", "--path", "bogus"}).error.find(
              "invalid path") != std::string::npos);
    CHECK(parse_command_line({"--suite", "acceptance", "--backend", "dx12", "--output", "out", "--path"}).error.find(
              "requires a value") != std::string::npos);
}
