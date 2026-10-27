#include "engine/benchmark/report.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include <cstdlib>
#include <string_view>
#include <thread>

namespace engine::bench {
namespace {

using Json = nlohmann::json;

Json summary_json(const std::optional<Summary>& summary) {
    if (!summary.has_value()) {
        return nullptr;
    }
    return {{"count", summary->count}, {"mean", summary->mean}, {"p50", summary->p50}, {"p95", summary->p95},
            {"p99", summary->p99}, {"max", summary->max}};
}

Json optional_bytes(const std::optional<std::uint64_t>& bytes) {
    return bytes.has_value() ? Json(*bytes) : Json(nullptr);
}

Json run_json(const RunRecord& run) {
    Json checks = Json::array();
    for (const Check& check : run.checks) {
        checks.push_back({{"name", check.name}, {"passed", check.passed}, {"detail", check.detail}});
    }
    Json counters = Json::object();
    for (const auto& [name, value] : run.counters) {
        counters[name] = value;
    }
    return {
        {"scene", run.scene},
        {"profile", run.profile},
        {"backend", run.backend},
        {"repeat", run.repeat},
        {"status", std::string(to_string(run.status))},
        {"reason", run.reason},
        {"frames_started", run.frames_started},
        {"measured_frames", run.frames.cpu_ms.size()},
        {"cpu_ms", summary_json(run.cpu)},
        {"gpu_ms", summary_json(run.gpu)},
        {"frame_ms", summary_json(run.frame)},
        {"checks", checks},
        {"residency", {{"gpu_bytes", optional_bytes(run.gpu_resident_bytes)}, {"host_working_set_bytes", optional_bytes(run.host_working_set_bytes)}}},
        {"counters", counters},
        {"path", run.path.empty() ? Json(nullptr) : Json(run.path)},
        {"features", run.features},
        {"raw_file", run.raw_file.empty() ? Json(nullptr) : Json(run.raw_file)},
    };
}

Json host_json(const HostInfo& host) {
    return {
        {"os", host.os},
        {"os_version", host.os_version},
        {"cpu", host.cpu},
        {"logical_cores", host.logical_cores},
        {"memory_bytes", optional_bytes(host.memory_bytes)},
        {"compiler", host.compiler},
        {"build_type", host.build_type},
        {"adapter", host.adapter},
        {"validation_layers", host.validation_layers},
    };
}

// Raw frame values for one repeat: frame index, CPU interval, and GPU span when measured.
std::string raw_csv(const RunRecord& run) {
    std::string text = "frame,cpu_ms,gpu_ms\n";
    char line[96];
    for (std::size_t i = 0; i < run.frames.cpu_ms.size(); ++i) {
        if (run.frames.gpu_present) {
            std::snprintf(line, sizeof line, "%zu,%.6f,%.6f\n", i, run.frames.cpu_ms[i], run.frames.gpu_ms[i]);
        } else {
            std::snprintf(line, sizeof line, "%zu,%.6f,\n", i, run.frames.cpu_ms[i]);
        }
        text += line;
    }
    return text;
}

}  // namespace

std::string_view to_string(RunStatus status) noexcept {
    switch (status) {
        case RunStatus::Passed: return "passed";
        case RunStatus::Failed: return "failed";
        case RunStatus::Unsupported: return "unsupported";
        case RunStatus::Missing: return "missing";
        case RunStatus::Incomplete: return "incomplete";
    }
    return "incomplete";
}

Totals totals(const Report& report) {
    Totals result;
    for (const RunRecord& run : report.runs) {
        switch (run.status) {
            case RunStatus::Passed: ++result.passed; break;
            case RunStatus::Failed: ++result.failed; break;
            case RunStatus::Unsupported: ++result.unsupported; break;
            case RunStatus::Missing: ++result.missing; break;
            case RunStatus::Incomplete: ++result.incomplete; break;
        }
    }
    return result;
}

int exit_code(const Report& report) {
    constexpr int kExitOk = 0;
    constexpr int kExitThresholdFailed = 1;
    constexpr int kExitNotVerified = 3;
    const Totals count = totals(report);
    if (count.failed > 0) {
        return kExitThresholdFailed;
    }
    // An empty report is not a success: nothing ran, so nothing was verified.
    if (report.runs.empty() || count.unsupported > 0 || count.missing > 0 || count.incomplete > 0) {
        return kExitNotVerified;
    }
    return kExitOk;
}

core::Result<core::Unit, std::string> write_report(const Report& report, const std::filesystem::path& dir) {
    std::error_code error;
    std::filesystem::create_directories(dir / "raw", error);
    if (error) {
        return core::failure("cannot create " + dir.string() + ": " + error.message());
    }

    Json runs = Json::array();
    for (const RunRecord& run : report.runs) {
        Json entry = run_json(run);
        if (!run.raw_file.empty()) {
            std::ofstream out(dir / run.raw_file, std::ios::binary);
            const std::string csv = raw_csv(run);
            out << csv;
            if (!out) {
                return core::failure("cannot write " + (dir / run.raw_file).string());
            }
        }
        runs.push_back(std::move(entry));
    }

    const Totals count = totals(report);
    Json workloads = Json::object();
    for (const auto& [name, json] : report.workloads) {
        workloads[name] = Json::parse(json);
    }
    Json profiles = Json::object();
    for (const auto& [name, json] : report.profiles) {
        profiles[name] = Json::parse(json);
    }
    const Json document = {
        {"schema", "engine_bench.report"},
        {"schema_version", 1},
        {"run_id", report.run_id},
        {"suite", report.suite},
        {"backend", report.backend},
        {"contract", report.contract},
        {"exit_code", exit_code(report)},
        {"manifest", {{"version", report.manifest_version}, {"sha256", report.manifest_sha256}}},
        {"timing", {{"warmup_s", report.timing.warmup_s}, {"duration_s", report.timing.duration_s}, {"repeats", report.timing.repeats}}},
        {"host", host_json(report.host)},
        {"totals", {{"passed", count.passed}, {"failed", count.failed}, {"unsupported", count.unsupported}, {"missing", count.missing}, {"incomplete", count.incomplete}}},
        {"workloads", workloads},
        {"profiles", profiles},
        {"runs", runs},
    };
    std::ofstream out(dir / "report.json", std::ios::binary);
    out << document.dump(2) << '\n';
    if (!out) {
        return core::failure("cannot write " + (dir / "report.json").string());
    }
    return core::success(core::Unit{});
}

HostInfo describe_host(const std::string& adapter, bool validation_layers) {
    HostInfo host;
    host.adapter = adapter;
    host.validation_layers = validation_layers;
    host.logical_cores = std::thread::hardware_concurrency();
#ifdef ENGINE_BENCH_BUILD_TYPE
    host.build_type = ENGINE_BENCH_BUILD_TYPE;
#else
    host.build_type = "unknown";
#endif
#if defined(_MSC_VER)
    host.compiler = "msvc " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    host.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    host.compiler = std::string("gcc ") + __VERSION__;
#else
    host.compiler = "unknown";
#endif

#if defined(_WIN32)
    host.os = "windows";
    host.os_version = "not recorded by engine_bench";
    // _dupenv_s instead of getenv, which MSVC reports as deprecated under /WX.
    char* cpu_name = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&cpu_name, &length, "PROCESSOR_IDENTIFIER") == 0 && cpu_name != nullptr) {
        host.cpu = cpu_name;
    }
    std::free(cpu_name);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory) != 0) {
        host.memory_bytes = memory.ullTotalPhys;
    }
#else
    utsname info{};
    if (uname(&info) == 0) {
        host.os = info.sysname;
        host.os_version = info.release;
    }
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.rfind("model name", 0) == 0 && host.cpu.empty()) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                host.cpu = line.substr(colon + 2);
            }
        }
    }
    std::ifstream meminfo("/proc/meminfo");
    for (std::string line; std::getline(meminfo, line);) {
        if (line.rfind("MemTotal:", 0) == 0) {
            host.memory_bytes = std::strtoull(line.c_str() + 9, nullptr, 10) * 1024ull;
        }
    }
#endif
    return host;
}

}  // namespace engine::bench
