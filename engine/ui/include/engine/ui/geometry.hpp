#pragma once

// Geometry and placement of the game UI (US-058). Positions are in canvas units: the canvas is the viewport divided by the UI scale,
// so a layout written against the reference size stays the same at every window size.

#include <cstdint>

namespace engine::ui {

struct Rgba8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

// A rectangle in canvas units. Edges are half-open for hit testing.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    [[nodiscard]] float right() const noexcept {
        return x + w;
    }
    [[nodiscard]] float bottom() const noexcept {
        return y + h;
    }
    [[nodiscard]] bool contains(float px, float py) const noexcept {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

// A rectangle in pixels of the output, half-open: [x0, x1) by [y0, y1).
struct PixelRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    [[nodiscard]] bool empty() const noexcept {
        return x1 <= x0 || y1 <= y0;
    }
};

// Margins from the edges of the anchor box, in canvas units.
struct Insets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// Anchor box of an element, as fractions of its parent's size: (0, 0) is the parent's top left, (1, 1) its bottom right.
struct Anchors {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

// Where an element sits. The rectangle is the anchor box, moved by `offsets.left`/`top` at its min edge and by
// `offsets.right`/`bottom` at its max edge. A point anchor (min == max) gives a fixed-size rectangle.
struct Placement {
    Anchors anchors{};
    Insets offsets{};
};

enum class Anchor : std::uint8_t {
    TopLeft,
    TopCentre,
    TopRight,
    CentreLeft,
    Centre,
    CentreRight,
    BottomLeft,
    BottomCentre,
    BottomRight,
};

// A w x h box whose top left is at the anchor point plus (x, y). A negative x or y pulls it inside an edge.
[[nodiscard]] Placement place(Anchor anchor, float x, float y, float w, float h) noexcept;

// Fills the parent, inset by the margins on each side.
[[nodiscard]] Placement stretch(Insets margins) noexcept;

// The rectangle of `placement` inside `parent`.
[[nodiscard]] Rect resolve(const Placement& placement, const Rect& parent) noexcept;

}  // namespace engine::ui
