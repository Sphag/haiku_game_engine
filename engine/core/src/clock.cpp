#include "engine/core/clock.hpp"

#include <chrono>

namespace engine::core {

units::Seconds MonotonicClock::now() noexcept {
    return std::chrono::duration_cast<units::Seconds>(std::chrono::steady_clock::now().time_since_epoch());
}

}  // namespace engine::core
