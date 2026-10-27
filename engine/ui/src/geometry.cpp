#include "engine/ui/geometry.hpp"

namespace engine::ui {

namespace {

// Fraction of the parent along each axis for an anchor preset: 0 at the start edge, 0.5 at the centre, 1 at the end edge.
void preset_fractions(Anchor anchor, float& fx, float& fy) noexcept {
    switch (anchor) {
        case Anchor::TopLeft:
            fx = 0.0f;
            fy = 0.0f;
            break;
        case Anchor::TopCentre:
            fx = 0.5f;
            fy = 0.0f;
            break;
        case Anchor::TopRight:
            fx = 1.0f;
            fy = 0.0f;
            break;
        case Anchor::CentreLeft:
            fx = 0.0f;
            fy = 0.5f;
            break;
        case Anchor::Centre:
            fx = 0.5f;
            fy = 0.5f;
            break;
        case Anchor::CentreRight:
            fx = 1.0f;
            fy = 0.5f;
            break;
        case Anchor::BottomLeft:
            fx = 0.0f;
            fy = 1.0f;
            break;
        case Anchor::BottomCentre:
            fx = 0.5f;
            fy = 1.0f;
            break;
        case Anchor::BottomRight:
            fx = 1.0f;
            fy = 1.0f;
            break;
    }
}

}  // namespace

Placement place(Anchor anchor, float x, float y, float w, float h) noexcept {
    float fx = 0.0f;
    float fy = 0.0f;
    preset_fractions(anchor, fx, fy);
    Placement placement;
    placement.anchors = Anchors{fx, fy, fx, fy};
    placement.offsets = Insets{x, y, x + w, y + h};
    return placement;
}

Placement stretch(Insets margins) noexcept {
    Placement placement;
    placement.anchors = Anchors{0.0f, 0.0f, 1.0f, 1.0f};
    placement.offsets = Insets{margins.left, margins.top, -margins.right, -margins.bottom};
    return placement;
}

Rect resolve(const Placement& placement, const Rect& parent) noexcept {
    const float x0 = parent.x + placement.anchors.min_x * parent.w + placement.offsets.left;
    const float y0 = parent.y + placement.anchors.min_y * parent.h + placement.offsets.top;
    const float x1 = parent.x + placement.anchors.max_x * parent.w + placement.offsets.right;
    const float y1 = parent.y + placement.anchors.max_y * parent.h + placement.offsets.bottom;
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace engine::ui
