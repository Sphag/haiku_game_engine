#pragma once

#include "engine/platform/window.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace engine::platform {

inline constexpr std::uint32_t kBaseDpi = 96;

// Windows reports DPI as 96 at 100% scale. A reported value of 0 (unknown) is treated as 100%.
[[nodiscard]] constexpr double dpi_scale_from_dpi(std::uint32_t dpi) noexcept {
    return dpi == 0 ? 1.0 : static_cast<double>(dpi) / static_cast<double>(kBaseDpi);
}

// Framebuffer pixels for a logical size, rounded to the nearest pixel. Values that do not fit in 32 bits saturate.
[[nodiscard]] inline FramebufferSize framebuffer_from_logical(LogicalSize logical, double scale) noexcept {
    const auto to_pixels = [scale](double value) -> std::uint32_t {
        const double pixels = std::round(value * scale);
        if (!(pixels > 0.0)) {
            return 0;
        }
        if (pixels >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return static_cast<std::uint32_t>(pixels);
    };
    return {to_pixels(logical.width), to_pixels(logical.height)};
}

// Logical size for a framebuffer. A non-positive scale is treated as 100%.
[[nodiscard]] inline LogicalSize logical_from_framebuffer(FramebufferSize framebuffer, double scale) noexcept {
    const double divisor = scale > 0.0 ? scale : 1.0;
    return {static_cast<double>(framebuffer.width) / divisor, static_cast<double>(framebuffer.height) / divisor};
}

}  // namespace engine::platform
