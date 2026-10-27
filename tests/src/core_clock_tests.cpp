#include "engine/core/clock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using engine::core::MonotonicClock;
using engine::core::Stopwatch;

TEST_CASE("Monotonic clock never moves backwards", "[unit]") {
    auto previous = MonotonicClock::now();
    for (int i = 0; i < 1000; ++i) {
        const auto current = MonotonicClock::now();
        CHECK(current >= previous);
        previous = current;
    }
}

TEST_CASE("Monotonic clock reports seconds as a double-precision duration", "[unit]") {
    const auto before = MonotonicClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const double elapsed_seconds = (MonotonicClock::now() - before).count();

    CHECK(elapsed_seconds >= 0.004);
    CHECK(elapsed_seconds < 10.0);
}

TEST_CASE("Stopwatch measures elapsed time and restarts", "[unit]") {
    Stopwatch stopwatch;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto first = stopwatch.elapsed();

    CHECK(first.count() >= 0.004);

    stopwatch.restart();

    CHECK(stopwatch.elapsed() < first);
}
