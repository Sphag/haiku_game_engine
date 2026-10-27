#include "engine/ui/ui.hpp"

#include "engine/platform/input_router.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace engine::ui {

namespace {

constexpr Rgba8 kFocusColour{255, 204, 0, 255};
constexpr float kFocusThickness = 2.0f;  // canvas units
// Positions past this many pixels are clamped before the int conversion, so a huge canvas offset cannot overflow.
constexpr float kPixelLimit = 1.0e6f;

float clamp_to(float value, float low, float high) noexcept {
    return std::min(std::max(value, low), high);
}

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool valid_placement(const Placement& placement) noexcept {
    return finite(placement.anchors.min_x) && finite(placement.anchors.min_y) && finite(placement.anchors.max_x) &&
           finite(placement.anchors.max_y) && finite(placement.offsets.left) && finite(placement.offsets.top) &&
           finite(placement.offsets.right) && finite(placement.offsets.bottom);
}

bool is_interactive(ElementKind kind) noexcept {
    return kind == ElementKind::Button || kind == ElementKind::Slider;
}

// A colour darkened to `numerator / denominator` of its channels; alpha is kept.
Rgba8 darken(Rgba8 colour, std::uint32_t numerator, std::uint32_t denominator) noexcept {
    colour.r = static_cast<std::uint8_t>(colour.r * numerator / denominator);
    colour.g = static_cast<std::uint8_t>(colour.g * numerator / denominator);
    colour.b = static_cast<std::uint8_t>(colour.b * numerator / denominator);
    return colour;
}

}  // namespace

struct Ui::Element {
    ElementSpec spec;
    ElementId parent = kNoElement;
    std::vector<ElementId> children;
    Rect rect;
    bool alive = true;
    bool shown = false;
};

Ui::Ui() = default;
Ui::~Ui() = default;
Ui::Ui(Ui&&) noexcept = default;
Ui& Ui::operator=(Ui&&) noexcept = default;

void Ui::set_font(Font font) {
    font_ = std::make_shared<const Font>(std::move(font));
}

core::Result<core::Unit, UiError> Ui::set_scaling(const UiScaling& scaling) {
    if (!finite(scaling.reference_width) || !finite(scaling.reference_height) || !finite(scaling.user_scale) ||
        scaling.reference_width <= 0.0f || scaling.reference_height <= 0.0f || scaling.user_scale <= 0.0f) {
        return core::failure(UiError::InvalidArgument);
    }
    scaling_ = scaling;
    relayout();
    return core::success();
}

Rect Ui::canvas() const noexcept {
    return Rect{0.0f, 0.0f, static_cast<float>(viewport_width_) / scale_, static_cast<float>(viewport_height_) / scale_};
}

void Ui::set_viewport(std::uint32_t width, std::uint32_t height) {
    viewport_width_ = width;
    viewport_height_ = height;
    if (!pointer_placed_) {
        pointer_x_ = static_cast<float>(width) * 0.5f;
        pointer_y_ = static_cast<float>(height) * 0.5f;
    }
    set_pointer(pointer_x_, pointer_y_);
    relayout();
}

void Ui::set_pointer(float x, float y) {
    if (!finite(x) || !finite(y)) {
        return;
    }
    pointer_x_ = clamp_to(x, 0.0f, static_cast<float>(viewport_width_));
    pointer_y_ = clamp_to(y, 0.0f, static_cast<float>(viewport_height_));
    pointer_placed_ = true;
}

void Ui::set_pointer_speed(float pixels_per_count) noexcept {
    if (finite(pixels_per_count) && pixels_per_count > 0.0f) {
        pointer_speed_ = pixels_per_count;
    }
}

void Ui::relayout() {
    scale_ = 1.0f;
    if (viewport_width_ > 0 && viewport_height_ > 0) {
        const float fit_x = static_cast<float>(viewport_width_) / scaling_.reference_width;
        const float fit_y = static_cast<float>(viewport_height_) / scaling_.reference_height;
        scale_ = std::min(fit_x, fit_y) * scaling_.user_scale;
    }
    const Rect root = canvas();
    for (const ElementId id : roots_) {
        layout_element(id, root, true);
    }
}

void Ui::layout_element(ElementId id, const Rect& parent, bool parent_shown) {
    Element* element = find(id);
    if (element == nullptr) {
        return;
    }
    element->rect = resolve(element->spec.placement, parent);
    element->shown = parent_shown && element->spec.visible;
    for (const ElementId child : element->children) {
        layout_element(child, element->rect, element->shown);
    }
}

core::Result<core::Unit, UiError> Ui::add_image(std::string name, Surface image) {
    if (images_.count(name) != 0) {
        return core::failure(UiError::DuplicateImage);
    }
    images_.emplace(std::move(name), std::move(image));
    return core::success();
}

core::Result<ElementId, UiError> Ui::add(ElementId parent, ElementSpec spec) {
    if (parent != kNoElement) {
        const Element* container = find(parent);
        if (container == nullptr || container->spec.kind != ElementKind::Panel) {
            return core::failure(UiError::InvalidParent);
        }
    }
    if (!valid_placement(spec.placement)) {
        return core::failure(UiError::InvalidArgument);
    }
    switch (spec.kind) {
        case ElementKind::Panel:
            break;
        case ElementKind::Image:
            if (images_.count(spec.image) == 0) {
                return core::failure(UiError::UnknownImage);
            }
            break;
        case ElementKind::Text:
        case ElementKind::Button:
            if (!finite(spec.text_size) || spec.text_size <= 0.0f) {
                return core::failure(UiError::InvalidArgument);
            }
            break;
        case ElementKind::Slider:
            if (!finite(spec.minimum) || !finite(spec.maximum) || !finite(spec.step) || !finite(spec.value) ||
                spec.minimum >= spec.maximum || spec.step <= 0.0f) {
                return core::failure(UiError::InvalidArgument);
            }
            spec.value = clamp_to(spec.value, spec.minimum, spec.maximum);
            break;
    }

    Element element;
    element.spec = std::move(spec);
    element.parent = parent;
    elements_.push_back(std::move(element));
    const auto id = static_cast<ElementId>(elements_.size());
    if (parent == kNoElement) {
        roots_.push_back(id);
    } else {
        find(parent)->children.push_back(id);
    }
    relayout();
    return core::success(id);
}

core::Result<core::Unit, UiError> Ui::remove(ElementId id) {
    Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    std::vector<ElementId>& siblings = element->parent == kNoElement ? roots_ : find(element->parent)->children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
    kill_subtree(id);
    relayout();
    return core::success();
}

void Ui::kill_subtree(ElementId id) {
    Element* element = find(id);
    if (element == nullptr) {
        return;
    }
    element->alive = false;
    if (focus_ == id) {
        focus_ = kNoElement;
    }
    if (pressed_ == id) {
        pressed_ = kNoElement;
    }
    if (dragging_ == id) {
        dragging_ = kNoElement;
    }
    const std::vector<ElementId> children = element->children;
    for (const ElementId child : children) {
        kill_subtree(child);
    }
}

core::Result<core::Unit, UiError> Ui::set_placement(ElementId id, const Placement& placement) {
    Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    if (!valid_placement(placement)) {
        return core::failure(UiError::InvalidArgument);
    }
    element->spec.placement = placement;
    relayout();
    return core::success();
}

core::Result<core::Unit, UiError> Ui::set_label(ElementId id, std::string label) {
    Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    if (element->spec.kind != ElementKind::Text && element->spec.kind != ElementKind::Button) {
        return core::failure(UiError::InvalidArgument);
    }
    element->spec.label = std::move(label);
    return core::success();
}

core::Result<core::Unit, UiError> Ui::set_visible(ElementId id, bool visible) {
    Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    element->spec.visible = visible;
    relayout();
    return core::success();
}

core::Result<core::Unit, UiError> Ui::set_enabled(ElementId id, bool enabled) {
    Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    element->spec.enabled = enabled;
    if (!enabled) {
        if (focus_ == id) {
            focus_ = kNoElement;
        }
        if (pressed_ == id) {
            pressed_ = kNoElement;
        }
        if (dragging_ == id) {
            dragging_ = kNoElement;
        }
    }
    return core::success();
}

core::Result<core::Unit, UiError> Ui::set_value(ElementId id, float value) {
    Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    if (element->spec.kind != ElementKind::Slider || !finite(value)) {
        return core::failure(UiError::InvalidArgument);
    }
    element->spec.value = clamp_to(value, element->spec.minimum, element->spec.maximum);
    return core::success();
}

core::Result<Rect, UiError> Ui::rect(ElementId id) const {
    const Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    return core::success(element->rect);
}

core::Result<float, UiError> Ui::value(ElementId id) const {
    const Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    if (element->spec.kind != ElementKind::Slider) {
        return core::failure(UiError::InvalidArgument);
    }
    return core::success(element->spec.value);
}

bool Ui::shown(ElementId id) const {
    const Element* element = find(id);
    return element != nullptr && element->shown;
}

core::Result<core::Unit, UiError> Ui::set_focus(ElementId id) {
    if (id == kNoElement) {
        focus_ = kNoElement;
        return core::success();
    }
    const Element* element = find(id);
    if (element == nullptr) {
        return core::failure(UiError::UnknownElement);
    }
    if (!is_interactive(element->spec.kind) || !element->spec.enabled || !element->shown) {
        return core::failure(UiError::InvalidArgument);
    }
    focus_ = id;
    return core::success();
}

ElementId Ui::hit_test(float x, float y) const {
    const float cx = x / scale_;
    const float cy = y / scale_;
    ElementId top = kNoElement;
    for (const ElementId id : roots_) {
        collect_hits(id, cx, cy, top);
    }
    return top;
}

void Ui::collect_hits(ElementId id, float cx, float cy, ElementId& top) const {
    const Element* element = find(id);
    if (element == nullptr || !element->shown) {
        return;
    }
    const ElementSpec& spec = element->spec;
    const bool target = is_interactive(spec.kind) ? spec.enabled : (spec.kind == ElementKind::Panel && spec.blocks_pointer);
    if (target && element->rect.contains(cx, cy)) {
        top = id;  // later in draw order is on top, and the walk is in draw order
    }
    for (const ElementId child : element->children) {
        collect_hits(child, cx, cy, top);
    }
}

void Ui::collect_focusable(ElementId id, std::vector<ElementId>& out) const {
    const Element* element = find(id);
    if (element == nullptr || !element->shown) {
        return;
    }
    if (is_interactive(element->spec.kind) && element->spec.enabled) {
        out.push_back(id);
    }
    for (const ElementId child : element->children) {
        collect_focusable(child, out);
    }
}

void Ui::move_focus(int step) {
    std::vector<ElementId> order;
    for (const ElementId id : roots_) {
        collect_focusable(id, order);
    }
    if (order.empty()) {
        focus_ = kNoElement;
        return;
    }
    const auto count = static_cast<int>(order.size());
    int index = step > 0 ? -1 : count;
    for (int i = 0; i < count; ++i) {
        if (order[static_cast<std::size_t>(i)] == focus_) {
            index = i;
            break;
        }
    }
    const int next = ((index + step) % count + count) % count;
    focus_ = order[static_cast<std::size_t>(next)];
}

void Ui::move_focus_spatial(int dx, int dy) {
    if (focus_ == kNoElement) {
        move_focus(1);
        return;
    }
    const Element* current = find(focus_);
    if (current == nullptr) {
        focus_ = kNoElement;
        return;
    }
    const float cx = current->rect.x + current->rect.w * 0.5f;
    const float cy = current->rect.y + current->rect.h * 0.5f;
    std::vector<ElementId> candidates;
    for (const ElementId id : roots_) {
        collect_focusable(id, candidates);
    }
    ElementId best = kNoElement;
    float best_score = std::numeric_limits<float>::max();
    for (const ElementId id : candidates) {
        if (id == focus_) {
            continue;
        }
        const Rect& other = find(id)->rect;
        const float ox = other.x + other.w * 0.5f;
        const float oy = other.y + other.h * 0.5f;
        const float along = static_cast<float>(dx) * (ox - cx) + static_cast<float>(dy) * (oy - cy);
        if (along <= 0.0f) {
            continue;  // not in the direction of the arrow
        }
        // Off-axis distance counts double, so a neighbour straight ahead beats a nearer one to the side.
        const float across = std::fabs(dx != 0 ? oy - cy : ox - cx);
        const float score = along + 2.0f * across;
        if (score < best_score) {
            best_score = score;
            best = id;
        }
    }
    if (best != kNoElement) {
        focus_ = best;
    }
}

void Ui::activate(ElementId id) {
    const Element* element = find(id);
    if (element == nullptr || element->spec.kind != ElementKind::Button || !element->spec.enabled) {
        return;
    }
    // Copied first: the callback may remove or add elements, which can move the vector.
    const std::function<void()> callback = element->spec.on_click;
    if (callback) {
        callback();
    }
}

void Ui::nudge_slider(ElementId id, float direction) {
    Element* element = find(id);
    if (element == nullptr || element->spec.kind != ElementKind::Slider || !element->spec.enabled) {
        return;
    }
    const float before = element->spec.value;
    const float after = clamp_to(before + direction * element->spec.step, element->spec.minimum, element->spec.maximum);
    if (after == before) {
        return;
    }
    element->spec.value = after;
    const std::function<void(float)> callback = element->spec.on_change;
    if (callback) {
        callback(after);
    }
}

void Ui::set_slider_from_pointer(ElementId id) {
    Element* element = find(id);
    if (element == nullptr || element->spec.kind != ElementKind::Slider || !element->spec.enabled || element->rect.w <= 0.0f) {
        return;
    }
    const float fraction = clamp_to((pointer_x_ / scale_ - element->rect.x) / element->rect.w, 0.0f, 1.0f);
    const float before = element->spec.value;
    const float after = element->spec.minimum + fraction * (element->spec.maximum - element->spec.minimum);
    if (after == before) {
        return;
    }
    element->spec.value = after;
    const std::function<void(float)> callback = element->spec.on_change;
    if (callback) {
        callback(after);
    }
}

UiInputResult Ui::handle(const std::vector<platform::InputEvent>& events) {
    for (const platform::InputEvent& event : events) {
        if (const auto* motion = std::get_if<platform::MouseMoved>(&event.data)) {
            set_pointer(pointer_x_ + static_cast<float>(motion->dx) * pointer_speed_, pointer_y_ + static_cast<float>(motion->dy) * pointer_speed_);
            if (active_ && dragging_ != kNoElement) {
                set_slider_from_pointer(dragging_);
            }
            continue;
        }
        if (!active_) {
            continue;  // a closed UI sees no keys, clicks, or pad buttons
        }
        if (const auto* key = std::get_if<platform::KeyPressed>(&event.data)) {
            switch (key->key) {
                case platform::Key::LeftShift:
                case platform::Key::RightShift:
                    shift_ = true;
                    break;
                case platform::Key::Tab:
                    move_focus(shift_ ? -1 : 1);
                    break;
                case platform::Key::Left:
                    if (focus_ != kNoElement && find(focus_)->spec.kind == ElementKind::Slider) {
                        nudge_slider(focus_, -1.0f);
                    } else {
                        move_focus_spatial(-1, 0);
                    }
                    break;
                case platform::Key::Right:
                    if (focus_ != kNoElement && find(focus_)->spec.kind == ElementKind::Slider) {
                        nudge_slider(focus_, 1.0f);
                    } else {
                        move_focus_spatial(1, 0);
                    }
                    break;
                case platform::Key::Up:
                    move_focus_spatial(0, -1);
                    break;
                case platform::Key::Down:
                    move_focus_spatial(0, 1);
                    break;
                case platform::Key::Enter:
                case platform::Key::Space:
                    activate(focus_);
                    break;
                default:
                    break;
            }
            continue;
        }
        if (const auto* key = std::get_if<platform::KeyReleased>(&event.data)) {
            if (key->key == platform::Key::LeftShift || key->key == platform::Key::RightShift) {
                shift_ = false;
            }
            continue;
        }
        if (const auto* button = std::get_if<platform::MouseButtonPressed>(&event.data)) {
            if (button->button != platform::MouseButton::Left) {
                continue;
            }
            const ElementId hit = hit_test(pointer_x_, pointer_y_);
            if (hit == kNoElement) {
                focus_ = kNoElement;
                continue;
            }
            const ElementKind kind = find(hit)->spec.kind;
            if (kind == ElementKind::Button) {
                pressed_ = hit;
                focus_ = hit;
            } else if (kind == ElementKind::Slider) {
                dragging_ = hit;
                focus_ = hit;
                set_slider_from_pointer(hit);
            }
            continue;
        }
        if (const auto* button = std::get_if<platform::MouseButtonReleased>(&event.data)) {
            if (button->button != platform::MouseButton::Left) {
                continue;
            }
            dragging_ = kNoElement;
            if (pressed_ != kNoElement) {
                const ElementId released = pressed_;
                pressed_ = kNoElement;
                if (hit_test(pointer_x_, pointer_y_) == released) {
                    activate(released);
                }
            }
            continue;
        }
        if (const auto* pad = std::get_if<platform::GamepadButtonPressed>(&event.data)) {
            switch (pad->button) {
                case platform::GamepadButton::A:
                    activate(focus_);
                    break;
                case platform::GamepadButton::DPadLeft:
                    move_focus_spatial(-1, 0);
                    break;
                case platform::GamepadButton::DPadRight:
                    move_focus_spatial(1, 0);
                    break;
                case platform::GamepadButton::DPadUp:
                    move_focus_spatial(0, -1);
                    break;
                case platform::GamepadButton::DPadDown:
                    move_focus_spatial(0, 1);
                    break;
                default:
                    break;
            }
            continue;
        }
    }

    UiInputResult result;
    if (!active_) {
        return result;
    }
    result.pointer_over_ui = pressed_ != kNoElement || dragging_ != kNoElement || hit_test(pointer_x_, pointer_y_) != kNoElement;
    result.captured = platform::kCaptureKeyboard | platform::kCaptureGamepad;
    if (result.pointer_over_ui) {
        result.captured |= platform::kCaptureMouse;
    }
    return result;
}

PixelRect Ui::to_pixels(const Rect& rect) const noexcept {
    const auto to_int = [](float value) {
        return static_cast<int>(clamp_to(value, -kPixelLimit, kPixelLimit));
    };
    PixelRect out;
    out.x0 = to_int(std::floor(rect.x * scale_));
    out.y0 = to_int(std::floor(rect.y * scale_));
    out.x1 = to_int(std::ceil(rect.right() * scale_));
    out.y1 = to_int(std::ceil(rect.bottom() * scale_));
    return out;
}

void Ui::draw_label(Surface& target, PixelRect area, const std::string& label, Rgba8 colour, float text_size, TextAlign align,
                    PixelRect clip) const {
    if (!font_ || label.empty()) {
        return;
    }
    const float size_px = text_size * scale_;
    const float width = font_->measure(label, size_px);
    const auto left = static_cast<float>(area.x0);
    const auto right = static_cast<float>(area.x1);
    float x = left;
    if (align == TextAlign::Centre) {
        x = left + (right - left - width) * 0.5f;
    } else if (align == TextAlign::End) {
        x = right - width;
    }
    const Font::Metrics metrics = font_->metrics(size_px);
    const auto top_edge = static_cast<float>(area.y0);
    const auto height = static_cast<float>(area.y1 - area.y0);
    const float baseline = top_edge + (height - (metrics.ascent + metrics.descent)) * 0.5f + metrics.ascent;
    font_->draw(target, label, size_px, x, baseline, colour, clip);
}

void Ui::draw_element(Surface& target, ElementId id, PixelRect clip) const {
    const Element* element = find(id);
    if (element == nullptr || !element->shown) {
        return;
    }
    const ElementSpec& spec = element->spec;
    const PixelRect area = to_pixels(element->rect);
    switch (spec.kind) {
        case ElementKind::Panel:
            target.fill(area, spec.colour, clip);
            break;
        case ElementKind::Image: {
            const auto image = images_.find(spec.image);
            if (image != images_.end()) {
                target.blit_scaled(image->second, area, spec.colour, clip);
            }
            break;
        }
        case ElementKind::Text:
            draw_label(target, area, spec.label, spec.text_colour, spec.text_size, spec.align, clip);
            break;
        case ElementKind::Button: {
            Rgba8 fill = spec.colour;
            if (pressed_ == id) {
                fill = darken(fill, 4, 5);
            }
            target.fill(area, fill, clip);
            if (focus_ == id) {
                const auto t = static_cast<int>(std::max(1.0f, std::round(kFocusThickness * scale_)));
                target.fill(PixelRect{area.x0, area.y0, area.x1, area.y0 + t}, kFocusColour, clip);
                target.fill(PixelRect{area.x0, area.y1 - t, area.x1, area.y1}, kFocusColour, clip);
                target.fill(PixelRect{area.x0, area.y0, area.x0 + t, area.y1}, kFocusColour, clip);
                target.fill(PixelRect{area.x1 - t, area.y0, area.x1, area.y1}, kFocusColour, clip);
            }
            draw_label(target, area, spec.label, spec.text_colour, spec.text_size, TextAlign::Centre, clip);
            break;
        }
        case ElementKind::Slider: {
            const int track_h = std::max(2, (area.y1 - area.y0) / 5);
            const int mid = (area.y0 + area.y1) / 2;
            const PixelRect track{area.x0, mid - track_h / 2, area.x1, mid - track_h / 2 + track_h};
            target.fill(track, spec.colour, clip);
            const float fraction = (spec.value - spec.minimum) / (spec.maximum - spec.minimum);
            const int knob_x = area.x0 + static_cast<int>(std::lround(fraction * static_cast<float>(area.x1 - area.x0)));
            target.fill(PixelRect{area.x0, track.y0, knob_x, track.y1}, spec.text_colour, clip);
            const int knob_w = std::max(4, (area.y1 - area.y0) / 3);
            target.fill(PixelRect{knob_x - knob_w / 2, area.y0, knob_x - knob_w / 2 + knob_w, area.y1}, spec.text_colour, clip);
            break;
        }
    }
    for (const ElementId child : element->children) {
        draw_element(target, child, clip);
    }
}

Surface Ui::render() const {
    Surface out(viewport_width_, viewport_height_);
    const PixelRect clip = out.bounds();
    for (const ElementId id : roots_) {
        draw_element(out, id, clip);
    }
    return out;
}

Ui::Element* Ui::find(ElementId id) noexcept {
    if (id == kNoElement || id > elements_.size()) {
        return nullptr;
    }
    Element& element = elements_[id - 1];
    return element.alive ? &element : nullptr;
}

const Ui::Element* Ui::find(ElementId id) const noexcept {
    if (id == kNoElement || id > elements_.size()) {
        return nullptr;
    }
    const Element& element = elements_[id - 1];
    return element.alive ? &element : nullptr;
}

}  // namespace engine::ui
