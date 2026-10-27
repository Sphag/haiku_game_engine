#pragma once

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/measure.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace engine::bench {

enum class RunStatus : std::uint8_t {
    Passed,
    Failed,        // measured, and a contract threshold was exceeded
    Unsupported,   // the backend, OS, or a required feature is not available here
    Missing,       // the scene's workload is not implemented yet
    Incomplete,    // the run did not produce what the contract needs
};

[[nodiscard]] std::string_view to_string(RunStatus status) noexcept;

struct RunRecord {
    std::string scene;
    std::string profile;
    std::string backend;
    int repeat = 0;                  // 1-based
    RunStatus status = RunStatus::Incomplete;
    std::string reason;
    std::uint64_t frames_started = 0;
    FrameTimes frames;               // raw values, written to the run's CSV
    std::optional<Summary> cpu;
    std::optional<Summary> gpu;
    std::optional<Summary> frame;    // max(cpu, gpu) per frame, the value the contract uses
    std::vector<Check> checks;
    std::optional<std::uint64_t> gpu_resident_bytes;
    std::optional<std::uint64_t> host_working_set_bytes;
    std::map<std::string, std::uint64_t> counters;
    std::string path;                // render path of the run (indexed, indirect, meshlets); empty for scenes without one
    std::map<std::string, std::string> features;  // what the device and the run used: "enabled", or why a feature is off
    std::string raw_file;            // path relative to the report folder; empty when no frames were kept
};

struct HostInfo {
    std::string os;
    std::string os_version;
    std::string cpu;
    unsigned logical_cores = 0;
    std::optional<std::uint64_t> memory_bytes;
    std::string compiler;
    std::string build_type;
    std::string adapter;             // empty when no device was opened
    bool validation_layers = false;
};

struct Report {
    std::string run_id;
    std::string suite;
    std::string backend;
    std::string manifest_sha256;
    std::string manifest_version;
    Timing timing;
    bool contract = false;           // true only for a Release, validation-free acceptance run
    HostInfo host;
    std::map<std::string, std::string> workloads;  // scene name -> its manifest workload, so a report can be reproduced
    std::map<std::string, std::string> profiles;   // profile name -> its manifest settings
    std::vector<RunRecord> runs;
};

struct Totals {
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t unsupported = 0;
    std::size_t missing = 0;
    std::size_t incomplete = 0;
};

[[nodiscard]] Totals totals(const Report& report);

// 0 when every run passed, 1 when a threshold failed, 3 when a run was unsupported, missing, or incomplete. A threshold failure
// takes precedence. Usage and manifest errors (2) are decided before a report exists.
[[nodiscard]] int exit_code(const Report& report);

// Writes report.json and one raw CSV per measured repeat under dir (created if absent).
[[nodiscard]] core::Result<core::Unit, std::string> write_report(const Report& report, const std::filesystem::path& dir);

// Host metadata for this machine. The adapter and validation fields come from the device the caller opened.
[[nodiscard]] HostInfo describe_host(const std::string& adapter, bool validation_layers);

}  // namespace engine::bench
