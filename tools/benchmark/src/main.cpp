// engine_bench (US-074): runs the benchmark suites from the versioned manifest and writes a report. A run counts as verified
// only when it measured every frame the contract needs on the named backend. Absent workloads, unavailable backends, and
// incomplete measurements are reported as such and exit 3; they are never counted as passed.

#include "engine/benchmark/cli.hpp"
#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/measure.hpp"
#include "engine/benchmark/physics_stress.hpp"
#include "engine/benchmark/streaming_stress.hpp"
#include "engine/benchmark/report.hpp"
#include "render_stress.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"
#include "engine/platform/platform.hpp"
#include "engine/rhi/device.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::bench::Manifest;
using engine::bench::Report;
using engine::bench::RunDef;
using engine::bench::RunRecord;
using engine::bench::RunStatus;
using engine::bench::SuiteDef;

constexpr int kExitUsage = 2;

struct BackendProbe {
    bool available = false;
    std::string reason;              // why the backend cannot run here, when it cannot
    std::string adapter;
    bool validation_layers = false;
    engine::rhi::CapabilityReport capabilities;
};

// Opens the backend once to learn whether it exists here and what the adapter supports. A missing loader, driver, display,
// or adapter is unavailable, not a failure. The benchmark never enables validation layers.
BackendProbe probe_backend(const std::string& backend) {
    BackendProbe probe;
#ifndef _WIN32
    if (backend == "dx12") {
        probe.reason = "the dx12 backend exists only on Windows";
        return probe;
    }
#endif
    const auto platform = engine::platform::Platform::create();
    if (!platform.ok()) {
        probe.reason = "no display (" + platform.error().detail + ")";
        return probe;
    }
    engine::rhi::DeviceDesc desc;
    desc.backend = backend == "dx12" ? engine::rhi::Backend::Dx12 : engine::rhi::Backend::Vulkan;
    desc.debug_layers = false;
    desc.allow_software_adapter = false;
    const auto device = engine::rhi::create_device(desc);
    if (!device.ok()) {
        probe.reason = device.error().message;
        return probe;
    }
    probe.available = true;
    probe.adapter = device.value()->capabilities().adapter_name;
    probe.validation_layers = device.value()->capabilities().validation_layers;
    probe.capabilities = device.value()->capabilities();
    return probe;
}

std::optional<engine::rhi::Feature> feature_from_name(const std::string& name) {
    if (name == "MeshShaders") return engine::rhi::Feature::MeshShaders;
    if (name == "IndirectCount") return engine::rhi::Feature::IndirectCount;
    if (name == "RayTracing") return engine::rhi::Feature::RayTracing;
    if (name == "TimestampQueries") return engine::rhi::Feature::TimestampQueries;
    return std::nullopt;
}

// Fills one repeat of one run. The status is decided in the order the contract needs: the backend, then the features the
// profile requires, then whether the scene has a workload, and only then the measurement.
void execute_run(const Manifest& manifest, const RunDef& def, const engine::bench::Timing& timing, bool acceptance,
                 const BackendProbe& probe, bool release_build, const engine::bench::Options& options, const std::string& backend,
                 const std::filesystem::path& output_dir, RunRecord& run) {
    const auto& profile = manifest.profiles.at(def.profile);
    const auto& scene = manifest.scenes.at(def.scene);

    if (acceptance && !probe.available) {
        run.status = RunStatus::Unsupported;
        run.reason = probe.reason;
        return;
    }
    for (const std::string& name : profile.requires_features) {
        const auto feature = feature_from_name(name);
        if (acceptance && (!feature.has_value() || !probe.capabilities.enabled(*feature))) {
            run.status = RunStatus::Unsupported;
            run.reason = "profile " + def.profile + " requires " + name + ", which this adapter does not support";
            return;
        }
    }
    if (def.scene == "physics_stress") {
        // The physics benchmark (US-077). The headless profile simulates on the CPU and has no frame-time contract. The visual profile
        // would need the render path with physics in the frame, which this build does not have, so it is reported as missing.
        if (profile.frame_contract) {
            run.status = RunStatus::Missing;
            run.reason = "physics_stress with " + def.profile + " needs the visual physics path, which is not implemented yet";
            return;
        }
        if (acceptance && !release_build) {
            run.status = RunStatus::Incomplete;
            run.reason = "acceptance is measured in a Release build; this build is not Release";
            return;
        }
        engine::bench::physics::RunSettings settings;
        settings.warmup_s = timing.warmup_s;
        settings.duration_s = timing.duration_s;
        engine::bench::physics::run_physics_stress(scene, settings, run);
        return;
    }
    if (def.scene == "streaming_stress") {
        // The streaming benchmark (US-078). Its headless profile runs the CPU stream only; the GPU profiles need the render path, which
        // this build does not have for this scene, so they are reported as missing.
        if (profile.frame_contract) {
            run.status = RunStatus::Missing;
            run.reason = "streaming_stress with " + def.profile + " needs the GPU streaming path, which is not implemented yet";
            return;
        }
        if (acceptance && !release_build) {
            run.status = RunStatus::Incomplete;
            run.reason = "acceptance is measured in a Release build; this build is not Release";
            return;
        }
        engine::bench::streaming::RunSettings settings;
        settings.warmup_s = timing.warmup_s;
        settings.duration_s = timing.duration_s;
        settings.paced = true;
        engine::bench::streaming::run_streaming_stress(scene, settings, run);
        return;
    }
    if (def.scene == "render_stress") {
        // The GPU scene (US-076). It needs a draw path from the command line, so each path is its own invocation and its own report.
        if (!acceptance) {
            run.status = RunStatus::Incomplete;
            run.reason = "render_stress runs only in the acceptance suite";
            return;
        }
        if (!release_build) {
            run.status = RunStatus::Incomplete;
            run.reason = "acceptance is measured in a Release build; this build is not Release";
            return;
        }
        if (options.path.empty()) {
            run.status = RunStatus::Incomplete;
            run.reason = "render_stress needs --path indexed, indirect, or meshlets";
            return;
        }
        engine::bench::render::RunSettings settings;
        settings.backend = backend;
        settings.path = *engine::bench::render::parse_path(options.path);
        settings.disable_mesh_shaders = options.disable_mesh_shaders;
        settings.shader_dir = ENGINE_BENCH_SHADER_DIR;
        settings.screenshot_dir = output_dir / "screenshots";
        settings.name = def.scene + "-" + def.profile + "-" + options.path + "-r" + std::to_string(run.repeat);
        settings.warmup_s = timing.warmup_s;
        settings.duration_s = timing.duration_s;
        engine::bench::render::run_render_stress(scene, def.profile, profile, settings, run);
        return;
    }
    if (!scene.builtin) {
        run.status = RunStatus::Missing;
        run.reason = "scene " + def.scene + " has no workload implementation yet";
        if (!scene.route.empty()) {
            run.reason += " (route " + scene.route + ": " + scene.route_status + ")";
        }
        return;
    }
    if (acceptance && !release_build) {
        run.status = RunStatus::Incomplete;
        run.reason = "acceptance is measured in a Release build; this build is not Release";
        return;
    }

    engine::bench::CalibrationWorkload workload(scene.seed, scene.counts.at("items"), scene.counts.at("rounds_per_frame"));
    const std::function<void()> frame = [&workload] { workload.frame(); };
    run.frames.cpu_ms = engine::bench::run_frames(timing.warmup_s, timing.duration_s, frame, run.frames_started);
    run.counters["frames_started"] = run.frames_started;
    run.counters["checksum"] = workload.checksum();
    if (run.frames.cpu_ms.empty()) {
        run.status = RunStatus::Incomplete;
        run.reason = "no frames were measured";
        return;
    }
    run.cpu = engine::bench::summarize(run.frames.cpu_ms);

    if (!acceptance || !profile.frame_contract) {
        // A self-check run has no performance contract, so it passes when it completes.
        run.status = RunStatus::Passed;
        run.reason = acceptance ? "headless diagnostic; no frame-time contract" : "self-check; no performance contract applies";
        return;
    }
    // Acceptance workloads will record GPU timestamps and residency here once they exist. Until then the contract is
    // reported as incomplete rather than evaluated on CPU time alone.
    engine::bench::RepeatEvidence evidence;
    evidence.frames = run.frames;
    const engine::bench::ContractResult contract = engine::bench::evaluate_contract(evidence);
    run.checks = contract.checks;
    run.reason = contract.reason;
    run.gpu_resident_bytes = evidence.gpu_resident_bytes;
    run.host_working_set_bytes = evidence.host_working_set_bytes;
    switch (contract.verdict) {
        case engine::bench::Verdict::Passed: run.status = RunStatus::Passed; break;
        case engine::bench::Verdict::Failed: run.status = RunStatus::Failed; break;
        case engine::bench::Verdict::Incomplete: run.status = RunStatus::Incomplete; break;
    }
}

void print_summary(const Report& report, const std::filesystem::path& dir) {
    const auto count = engine::bench::totals(report);
    std::cout << "engine_bench: " << report.run_id << " suite=" << report.suite << " backend=" << report.backend
              << " contract=" << (report.contract ? "yes" : "no") << "\n";
    for (const RunRecord& run : report.runs) {
        std::cout << "  " << run.scene << " / " << run.profile << " repeat " << run.repeat << ": "
                  << engine::bench::to_string(run.status);
        if (!run.reason.empty()) {
            std::cout << " (" << run.reason << ")";
        }
        std::cout << "\n";
    }
    std::cout << "passed=" << count.passed << " failed=" << count.failed << " unsupported=" << count.unsupported
              << " missing=" << count.missing << " incomplete=" << count.incomplete << "\n";
    std::cout << "report: " << (dir / "report.json").string() << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const auto parsed = engine::bench::parse_command_line(args);
    if (!parsed.error.empty()) {
        std::cerr << "engine_bench: " << parsed.error << "\n\n" << engine::bench::usage_text();
        return kExitUsage;
    }
    const engine::bench::Options& options = parsed.options;
    if (options.show_help) {
        std::cout << engine::bench::usage_text();
        return 0;
    }

    const std::string manifest_path = options.manifest.empty() ? ENGINE_BENCH_MANIFEST : options.manifest;
    const auto path = engine::core::Path::from_utf8(manifest_path);
    if (!path.ok()) {
        std::cerr << "engine_bench: manifest path is not valid UTF-8: " << manifest_path << "\n";
        return kExitUsage;
    }
    const auto text = engine::core::read_text_file(path.value());
    if (!text.ok()) {
        std::cerr << "engine_bench: cannot read manifest " << manifest_path << ": " << engine::core::describe(text.error()) << "\n";
        return kExitUsage;
    }
    const auto manifest = engine::bench::parse_manifest(text.value());
    if (!manifest.ok()) {
        std::cerr << "engine_bench: invalid manifest " << manifest_path << ": " << manifest.error() << "\n";
        return kExitUsage;
    }
    const auto suite_it = manifest.value().suites.find(options.suite);
    if (suite_it == manifest.value().suites.end()) {
        std::cerr << "engine_bench: the manifest has no suite '" << options.suite << "'\n";
        return kExitUsage;
    }

    const SuiteDef& suite = suite_it->second;
    const bool acceptance = options.suite == "acceptance";
    engine::bench::Timing timing = suite.timing;
    if (options.warmup_s) timing.warmup_s = *options.warmup_s;
    if (options.duration_s) timing.duration_s = *options.duration_s;
    if (options.repeats) timing.repeats = *options.repeats;

    const BackendProbe probe = acceptance ? probe_backend(options.backend) : BackendProbe{};
#ifdef ENGINE_BENCH_BUILD_TYPE
    const bool release_build = std::string_view(ENGINE_BENCH_BUILD_TYPE) == "Release";
#else
    const bool release_build = false;
#endif

    const std::filesystem::path output(options.output);
    Report report;
    report.run_id = output.filename().empty() ? output.parent_path().filename().string() : output.filename().string();
    report.suite = options.suite;
    report.backend = acceptance ? options.backend : "none";
    report.manifest_sha256 = manifest.value().sha256;
    report.manifest_version = "1";
    report.timing = timing;
    report.host = engine::bench::describe_host(probe.adapter, probe.validation_layers);
    report.contract = acceptance && release_build && !probe.validation_layers;
    for (const auto& [name, scene] : manifest.value().scenes) {
        report.workloads[name] = scene.definition_json;
    }
    for (const auto& [name, profile] : manifest.value().profiles) {
        report.profiles[name] = profile.settings_json;
    }

    for (const RunDef& def : suite.runs) {
        for (int repeat = 1; repeat <= timing.repeats; ++repeat) {
            RunRecord run;
            run.scene = def.scene;
            run.profile = def.profile;
            run.backend = report.backend;
            run.repeat = repeat;
            execute_run(manifest.value(), def, timing, acceptance, probe, release_build, options, report.backend, output, run);
            if (!run.frames.cpu_ms.empty()) {
                run.raw_file = "raw/" + def.scene + "-" + def.profile + "-" + report.backend + "-r" + std::to_string(repeat) + ".csv";
            }
            report.runs.push_back(std::move(run));
        }
    }
    if (report.runs.empty()) {
        std::cerr << "engine_bench: the suite has no runs\n";
    }

    const auto written = engine::bench::write_report(report, output);
    if (!written.ok()) {
        std::cerr << "engine_bench: " << written.error() << "\n";
        return kExitUsage;
    }
    print_summary(report, output);
    return engine::bench::exit_code(report);
}
