#pragma once

#include "engine/core/units.hpp"

namespace engine::core {

// Steady clock for measuring elapsed time. Readings have no meaning on their own; only differences do.
class MonotonicClock {
public:
    [[nodiscard]] static units::Seconds now() noexcept;
};

class Stopwatch {
public:
    Stopwatch() noexcept : start_(MonotonicClock::now()) {}

    void restart() noexcept {
        start_ = MonotonicClock::now();
    }

    [[nodiscard]] units::Seconds elapsed() const noexcept {
        return MonotonicClock::now() - start_;
    }

private:
    units::Seconds start_;
};

}  // namespace engine::core
