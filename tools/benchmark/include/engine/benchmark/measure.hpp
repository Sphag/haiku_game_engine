#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace engine::bench {

// Per-frame times in milliseconds. cpu_ms is the start-to-start interval of each measured frame; gpu_ms is the frame's
// timestamp span and is present only when the workload measures it.
struct FrameTimes {
    std::vector<double> cpu_ms;
    std::vector<double> gpu_ms;
    bool gpu_present = false;
};

struct Summary {
    std::size_t count = 0;
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

// Nearest-rank percentiles (perf::nearest_rank_index). An empty input gives a zero summary with count 0.
[[nodiscard]] Summary summarize(std::vector<double> values);

// Calls frame() until warmup_s + duration_s has elapsed. Returns the start-to-start interval of each frame that starts after
// the warmup, in milliseconds. frames_started counts every call, including warmup frames.
[[nodiscard]] std::vector<double> run_frames(double warmup_s, double duration_s, const std::function<void()>& frame,
                                             std::uint64_t& frames_started);

// Deterministic CPU workload for the harness self-check. Its checksum depends only on the seed and the parameters.
class CalibrationWorkload {
public:
    CalibrationWorkload(std::uint64_t seed, std::uint64_t items, std::uint64_t rounds_per_frame);

    void frame();
    [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }

private:
    std::vector<std::uint64_t> state_;
    std::uint64_t rounds_per_frame_;
    std::uint64_t checksum_ = 0;
};

struct Check {
    std::string name;
    bool passed = false;
    std::string detail;
};

enum class Verdict : std::uint8_t {
    Passed,
    Failed,       // a threshold was exceeded
    Incomplete,   // a measurement needed by the contract is missing, so nothing can be concluded
};

struct ContractResult {
    Verdict verdict = Verdict::Incomplete;
    std::vector<Check> checks;
    std::string reason;  // why the verdict is Incomplete, empty otherwise
};

// Everything one repeat measured. Residency is optional: a value that was not measured is reported as missing, never zero.
struct RepeatEvidence {
    FrameTimes frames;
    std::optional<std::uint64_t> gpu_resident_bytes;
    std::optional<std::uint64_t> host_working_set_bytes;
};

// The PRD performance contract for one repeat: p99 within 1000/60 ms, at least 99% of frames within it, at most 0.1% over
// 1000/30 ms, no frame over 100 ms, and residency within 12 GiB GPU and 16 GiB host. Acceptance uses max(cpu, gpu) per frame.
[[nodiscard]] ContractResult evaluate_contract(const RepeatEvidence& evidence);

}  // namespace engine::bench
