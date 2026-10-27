#include "engine/benchmark/measure.hpp"

#include "engine/core/clock.hpp"
#include "engine/core/performance_contract.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace engine::bench {
namespace {

constexpr double kWithinBudgetFraction = 0.99;  // at least this share of frames meets 1000/60 ms
constexpr std::uint64_t kGpuResidencyLimit = 12ull << 30;   // 12 GiB of GPU memory
constexpr std::uint64_t kHostResidencyLimit = 16ull << 30;  // 16 GiB host working set

Check threshold(std::string name, bool passed, std::string detail) {
    return Check{std::move(name), passed, std::move(detail)};
}

}  // namespace

Summary summarize(std::vector<double> values) {
    Summary summary;
    if (values.empty()) {
        return summary;
    }
    std::sort(values.begin(), values.end());
    summary.count = values.size();
    summary.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    summary.p50 = values[core::perf::nearest_rank_index(0.50, values.size())];
    summary.p95 = values[core::perf::nearest_rank_index(0.95, values.size())];
    summary.p99 = values[core::perf::nearest_rank_index(0.99, values.size())];
    summary.max = values.back();
    return summary;
}

std::vector<double> run_frames(double warmup_s, double duration_s, const std::function<void()>& frame, std::uint64_t& frames_started) {
    std::vector<double> starts;
    const core::Stopwatch clock;
    const double end_s = warmup_s + duration_s;
    for (;;) {
        const double now = clock.elapsed().count();
        if (now >= end_s) {
            break;
        }
        starts.push_back(now);
        frame();
    }
    frames_started = starts.size();

    // The interval of frame i runs from its start to the next start. The last frame has no successor and is not timed.
    std::vector<double> intervals;
    for (std::size_t i = 0; i + 1 < starts.size(); ++i) {
        if (starts[i] >= warmup_s) {
            intervals.push_back((starts[i + 1] - starts[i]) * 1000.0);
        }
    }
    return intervals;
}

CalibrationWorkload::CalibrationWorkload(std::uint64_t seed, std::uint64_t items, std::uint64_t rounds_per_frame)
    : state_(items), rounds_per_frame_(rounds_per_frame) {
    // splitmix64 fills the state from the seed, so the same seed gives the same values on every platform.
    std::uint64_t x = seed;
    for (auto& value : state_) {
        x += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        value = z ^ (z >> 31);
    }
}

void CalibrationWorkload::frame() {
    for (std::uint64_t round = 0; round < rounds_per_frame_; ++round) {
        for (std::size_t i = 0; i < state_.size(); ++i) {
            std::uint64_t value = state_[i] + i + round;
            value ^= value >> 13;
            value *= 0xFF51AFD7ED558CCDull;
            state_[i] = value;
            checksum_ ^= value + (checksum_ << 6) + (checksum_ >> 2);
        }
    }
}

ContractResult evaluate_contract(const RepeatEvidence& evidence) {
    ContractResult result;
    const FrameTimes& frames = evidence.frames;
    if (frames.cpu_ms.empty()) {
        result.verdict = Verdict::Incomplete;
        result.reason = "no measured frames";
        return result;
    }
    if (!frames.gpu_present || frames.gpu_ms.size() != frames.cpu_ms.size()) {
        result.verdict = Verdict::Incomplete;
        result.reason = "GPU frame timings are unavailable, so max(cpu, gpu) cannot be computed";
        return result;
    }

    std::vector<double> frame_ms(frames.cpu_ms.size());
    for (std::size_t i = 0; i < frame_ms.size(); ++i) {
        frame_ms[i] = std::max(frames.cpu_ms[i], frames.gpu_ms[i]);
    }
    const Summary summary = summarize(frame_ms);
    const double budget_ms = core::perf::kP99FrameBudget.count();
    const double rare_ms = core::perf::kRareFrameLimit.count();

    const auto within_budget = static_cast<double>(std::count_if(frame_ms.begin(), frame_ms.end(), [&](double ms) { return ms <= budget_ms; }));
    const auto rare = static_cast<double>(std::count_if(frame_ms.begin(), frame_ms.end(), [&](double ms) { return ms > rare_ms; }));
    const auto n = static_cast<double>(frame_ms.size());

    result.checks.push_back(threshold("p99 <= 1000/60 ms", summary.p99 <= budget_ms, std::to_string(summary.p99) + " ms"));
    result.checks.push_back(threshold("frames within 1000/60 ms >= 99%", within_budget / n >= kWithinBudgetFraction,
                                      std::to_string(within_budget / n * 100.0) + "%"));
    result.checks.push_back(threshold("frames over 1000/30 ms <= 0.1%", rare / n <= core::perf::kRareFrameFractionLimit,
                                      std::to_string(rare / n * 100.0) + "%"));
    result.checks.push_back(threshold("no frame over 100 ms", summary.max <= core::perf::kMaxFrameAfterWarmup.count(),
                                      std::to_string(summary.max) + " ms"));

    const bool timing_passed = std::all_of(result.checks.begin(), result.checks.end(), [](const Check& check) { return check.passed; });
    if (!evidence.gpu_resident_bytes.has_value() || !evidence.host_working_set_bytes.has_value()) {
        // A timing failure is reported as a failure even when residency is missing; it is never hidden as incomplete.
        result.verdict = timing_passed ? Verdict::Incomplete : Verdict::Failed;
        result.reason = "residency was not measured";
        return result;
    }
    result.checks.push_back(threshold("GPU residency <= 12 GiB", *evidence.gpu_resident_bytes <= kGpuResidencyLimit,
                                      std::to_string(*evidence.gpu_resident_bytes) + " bytes"));
    result.checks.push_back(threshold("host working set <= 16 GiB", *evidence.host_working_set_bytes <= kHostResidencyLimit,
                                      std::to_string(*evidence.host_working_set_bytes) + " bytes"));

    const bool all_passed = std::all_of(result.checks.begin(), result.checks.end(), [](const Check& check) { return check.passed; });
    result.verdict = all_passed ? Verdict::Passed : Verdict::Failed;
    return result;
}

}  // namespace engine::bench
