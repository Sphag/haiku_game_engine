#pragma once

#include "engine/core/units.hpp"

#include <cmath>
#include <cstddef>

// Fixed frame-time limits from the PRD performance contract. Benchmark code (US-074 onward) uses these values
// directly; they are not tuned per scene or per machine.
namespace engine::core::perf {

// p99 of frame times must not exceed 1000/60 ms in every repeat.
inline constexpr units::Milliseconds kP99FrameBudget{1000.0 / 60.0};
// At most 0.1% of frames may exceed 1000/30 ms.
inline constexpr units::Milliseconds kRareFrameLimit{1000.0 / 30.0};
inline constexpr double kRareFrameFractionLimit = 0.001;
// No frame may exceed 100 ms after warmup.
inline constexpr units::Milliseconds kMaxFrameAfterWarmup{100.0};

// 0-based index of the nearest-rank percentile in a sorted sample set: rank = ceil(p * n), index = rank - 1.
// Requires 0 < p <= 1 and sample_count > 0. Rounding of reported values does not change the comparison.
[[nodiscard]] inline std::size_t nearest_rank_index(double p, std::size_t sample_count) noexcept {
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sample_count)));
    if (rank <= 1) {
        return 0;
    }
    return rank >= sample_count ? sample_count - 1 : rank - 1;
}

}  // namespace engine::core::perf
