#pragma once

// The CPU raster of the game UI (US-058). A Surface holds RGBA8 with straight alpha, the layout the render layer's overlay takes
// (ImageRgba8 in display space). Every drawing call clips to the surface and blends source over destination.

#include "engine/ui/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::ui {

class Surface {
public:
    Surface() = default;
    // A transparent surface of the given size.
    Surface(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept {
        return pixels_;
    }
    // Moves the pixel bytes out. The surface is empty afterwards.
    [[nodiscard]] std::vector<std::uint8_t> take_pixels() noexcept;

    [[nodiscard]] Rgba8 at(std::uint32_t x, std::uint32_t y) const noexcept;

    // Source-over blend of one pixel. Pixels outside the surface are ignored.
    void blend(int x, int y, Rgba8 source) noexcept;

    // Source-over fill of a pixel rectangle, clipped to the surface and to `clip`.
    void fill(PixelRect area, Rgba8 colour, PixelRect clip) noexcept;

    // Blends a coverage mask (one byte per pixel, width x height) tinted by `colour`, clipped to `clip`.
    void blit_coverage(int x, int y, int width, int height, const std::uint8_t* coverage, Rgba8 colour, PixelRect clip) noexcept;

    // Draws `image` stretched over `area` with nearest-neighbour sampling, multiplied by `tint`, clipped to `clip`.
    void blit_scaled(const Surface& image, PixelRect area, Rgba8 tint, PixelRect clip) noexcept;

    // The surface's full bounds as a pixel rectangle.
    [[nodiscard]] PixelRect bounds() const noexcept;

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

// Intersection of two pixel rectangles.
[[nodiscard]] PixelRect intersect(PixelRect a, PixelRect b) noexcept;

}  // namespace engine::ui
