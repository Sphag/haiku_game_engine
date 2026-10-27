#pragma once

#include <chrono>

// Engine units (see docs/runtime-conventions.md):
//   length: meters (double in APIs, float in rendering math)
//   time:   seconds (std::chrono::duration<double>)
namespace engine::core::units {

using Seconds = std::chrono::duration<double>;
using Milliseconds = std::chrono::duration<double, std::milli>;

inline constexpr double kMetersPerCentimeter = 0.01;
inline constexpr double kMetersPerInch = 0.0254;
inline constexpr double kMetersPerFoot = 0.3048;

[[nodiscard]] constexpr double centimeters_to_meters(double centimeters) noexcept {
    return centimeters * kMetersPerCentimeter;
}

[[nodiscard]] constexpr double inches_to_meters(double inches) noexcept {
    return inches * kMetersPerInch;
}

[[nodiscard]] constexpr double feet_to_meters(double feet) noexcept {
    return feet * kMetersPerFoot;
}

}  // namespace engine::core::units
