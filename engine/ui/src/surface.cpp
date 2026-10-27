#include "engine/ui/surface.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace engine::ui {

namespace {

std::size_t offset_of(std::uint32_t width, int x, int y) {
    return (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
}

// Source-over of straight-alpha colours, written into the four bytes at `dst`.
void blend_into(std::uint8_t* dst, Rgba8 source) noexcept {
    const std::uint32_t sa = source.a;
    if (sa == 0) {
        return;
    }
    const std::uint32_t da = dst[3];
    if (sa == 255 || da == 0) {
        dst[0] = source.r;
        dst[1] = source.g;
        dst[2] = source.b;
        dst[3] = source.a;
        return;
    }
    // out_a = sa + da * (1 - sa); colour = (src * sa + dst * da * (1 - sa)) / out_a, all in 0..255 with rounding.
    const std::uint32_t inverse = 255 - sa;
    const std::uint32_t out_a = sa * 255 + da * inverse;  // scaled by 255
    const std::uint32_t channels[3] = {source.r, source.g, source.b};
    for (int c = 0; c < 3; ++c) {
        const std::uint32_t mixed = channels[c] * sa * 255 + dst[c] * da * inverse;
        dst[c] = static_cast<std::uint8_t>((mixed + out_a / 2) / out_a);
    }
    dst[3] = static_cast<std::uint8_t>((out_a + 127) / 255);
}

}  // namespace

Surface::Surface(std::uint32_t width, std::uint32_t height)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 4, 0) {}

std::vector<std::uint8_t> Surface::take_pixels() noexcept {
    std::vector<std::uint8_t> out = std::move(pixels_);
    pixels_.clear();
    width_ = 0;
    height_ = 0;
    return out;
}

Rgba8 Surface::at(std::uint32_t x, std::uint32_t y) const noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * width_ + x) * 4;
    return Rgba8{pixels_[i], pixels_[i + 1], pixels_[i + 2], pixels_[i + 3]};
}

PixelRect Surface::bounds() const noexcept {
    return PixelRect{0, 0, static_cast<int>(width_), static_cast<int>(height_)};
}

PixelRect intersect(PixelRect a, PixelRect b) noexcept {
    return PixelRect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

void Surface::blend(int x, int y, Rgba8 source) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_)) {
        return;
    }
    blend_into(pixels_.data() + offset_of(width_, x, y), source);
}

void Surface::fill(PixelRect area, Rgba8 colour, PixelRect clip) noexcept {
    const PixelRect target = intersect(intersect(area, clip), bounds());
    for (int y = target.y0; y < target.y1; ++y) {
        for (int x = target.x0; x < target.x1; ++x) {
            blend(x, y, colour);
        }
    }
}

void Surface::blit_coverage(int x, int y, int width, int height, const std::uint8_t* coverage, Rgba8 colour, PixelRect clip) noexcept {
    const PixelRect target = intersect(intersect(PixelRect{x, y, x + width, y + height}, clip), bounds());
    for (int py = target.y0; py < target.y1; ++py) {
        for (int px = target.x0; px < target.x1; ++px) {
            const std::uint32_t cov = coverage[static_cast<std::size_t>(py - y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px - x)];
            if (cov == 0) {
                continue;
            }
            Rgba8 source = colour;
            source.a = static_cast<std::uint8_t>((cov * colour.a + 127) / 255);
            blend(px, py, source);
        }
    }
}

void Surface::blit_scaled(const Surface& image, PixelRect area, Rgba8 tint, PixelRect clip) noexcept {
    if (image.width_ == 0 || image.height_ == 0 || area.empty()) {
        return;
    }
    const PixelRect target = intersect(intersect(area, clip), bounds());
    const auto dst_w = static_cast<std::int64_t>(area.x1 - area.x0);
    const auto dst_h = static_cast<std::int64_t>(area.y1 - area.y0);
    for (int y = target.y0; y < target.y1; ++y) {
        // Sample at the texel centre: the nearest source row for this destination row.
        const auto sy = static_cast<std::uint32_t>(((static_cast<std::int64_t>(y - area.y0) * 2 + 1) * image.height_) / (dst_h * 2));
        for (int x = target.x0; x < target.x1; ++x) {
            const auto sx = static_cast<std::uint32_t>(((static_cast<std::int64_t>(x - area.x0) * 2 + 1) * image.width_) / (dst_w * 2));
            const std::size_t i = (static_cast<std::size_t>(sy) * image.width_ + sx) * 4;
            Rgba8 source{image.pixels_[i], image.pixels_[i + 1], image.pixels_[i + 2], image.pixels_[i + 3]};
            source.r = static_cast<std::uint8_t>((source.r * tint.r + 127) / 255);
            source.g = static_cast<std::uint8_t>((source.g * tint.g + 127) / 255);
            source.b = static_cast<std::uint8_t>((source.b * tint.b + 127) / 255);
            source.a = static_cast<std::uint8_t>((source.a * tint.a + 127) / 255);
            blend(x, y, source);
        }
    }
}

}  // namespace engine::ui
