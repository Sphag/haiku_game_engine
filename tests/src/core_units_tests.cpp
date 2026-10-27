#include "engine/core/performance_contract.hpp"
#include "engine/core/units.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using engine::core::units::Milliseconds;
using engine::core::units::Seconds;

TEST_CASE("Length conversions go to meters", "[unit]") {
    CHECK(engine::core::units::centimeters_to_meters(250.0) == Catch::Approx(2.5));
    CHECK(engine::core::units::inches_to_meters(10.0) == Catch::Approx(0.254));
    CHECK(engine::core::units::feet_to_meters(3.0) == Catch::Approx(0.9144));
}

TEST_CASE("Time durations are seconds in double precision", "[unit]") {
    const Seconds from_ms = std::chrono::duration_cast<Seconds>(Milliseconds(250.0));

    CHECK(from_ms.count() == Catch::Approx(0.25));
}

TEST_CASE("Performance contract budgets match the PRD fixed values", "[unit]") {
    using engine::core::perf::kMaxFrameAfterWarmup;
    using engine::core::perf::kP99FrameBudget;
    using engine::core::perf::kRareFrameLimit;

    CHECK(kP99FrameBudget.count() == Catch::Approx(1000.0 / 60.0));
    CHECK(kRareFrameLimit.count() == Catch::Approx(1000.0 / 30.0));
    CHECK(kMaxFrameAfterWarmup.count() == Catch::Approx(100.0));
    CHECK(engine::core::perf::kRareFrameFractionLimit == Catch::Approx(0.001));
}

TEST_CASE("Nearest-rank percentile uses ceil(p * n) as a 1-based rank", "[unit]") {
    using engine::core::perf::nearest_rank_index;

    CHECK(nearest_rank_index(0.99, 100) == 98);
    CHECK(nearest_rank_index(0.5, 4) == 1);
    CHECK(nearest_rank_index(1.0, 5) == 4);
    CHECK(nearest_rank_index(0.001, 1000) == 0);
    CHECK(nearest_rank_index(0.99, 1) == 0);
}

TEST_CASE("Nearest-rank percentile selects the sample the contract compares", "[integration]") {
    // 100 frames: 99 at 16 ms and one at 40 ms. The 99th nearest-rank sample is still 16 ms.
    std::vector<double> frame_ms(99, 16.0);
    frame_ms.push_back(40.0);

    const double p99 = frame_ms[engine::core::perf::nearest_rank_index(0.99, frame_ms.size())];

    CHECK(p99 <= engine::core::perf::kP99FrameBudget.count());
}
