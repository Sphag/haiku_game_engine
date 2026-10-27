#pragma once

// The game UI (US-058): a tree of panels, images, text, buttons, and sliders laid out on a canvas with anchors. The canvas is the
// viewport divided by the UI scale (uniform, the smaller of the width and height ratios to the reference size, times a user scale), so
// a layout reads the same at every window size.
//
// Input. The UI consumes platform input events: pointer motion (device counts, added to a pointer that starts at the viewport centre
// and is clamped to it; set_pointer places it from an absolute cursor), mouse buttons, keys, and gamepad buttons. Tab and the arrow keys
// (or the D-pad) move focus among enabled buttons and sliders; Enter, Space, or A activates the focused button; the arrows change the
// focused slider. Only an active UI (set_active) handles input. handle() reports the device classes the UI owns, so the host passes them
// to InputRouter::set_capture(CaptureOwner::Ui, ...) and gameplay does not see them.
//
// Output. render() rasterizes the tree into a viewport-sized RGBA8 surface. The host composites it after post-processing, through
// PostProcessor::set_overlay. The UI never touches the GPU and does not need the editor.

#include "engine/core/result.hpp"
#include "engine/platform/input.hpp"
#include "engine/ui/font.hpp"
#include "engine/ui/geometry.hpp"
#include "engine/ui/surface.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine::ui {

using ElementId = std::uint32_t;  // 0 names no element; ids stay unique for the life of the Ui
inline constexpr ElementId kNoElement = 0;

enum class ElementKind : std::uint8_t {
    Panel,   // a filled rectangle; the only kind that holds children
    Image,   // a registered image stretched over the rectangle, tinted by `colour`
    Text,    // one line of `label`
    Button,  // a filled rectangle with a centred `label`; activated by click, Enter, Space, or A
    Slider,  // a horizontal track in [minimum, maximum]; changed by drag and by the arrows
};

enum class TextAlign : std::uint8_t { Start, Centre, End };

struct ElementSpec {
    ElementKind kind = ElementKind::Panel;
    Placement placement{};
    Rgba8 colour{255, 255, 255, 255};       // Panel fill, Image tint, Button fill, Slider track
    Rgba8 text_colour{0, 0, 0, 255};        // Text and Button label, Slider knob and fill
    std::string label;                      // Text and Button
    std::string image;                      // Image: a name registered with Ui::add_image
    float text_size = 24.0f;                // canvas units, so it scales with the UI
    TextAlign align = TextAlign::Centre;    // Text alignment inside the rectangle
    float value = 0.0f;                     // Slider
    float minimum = 0.0f;                   // Slider
    float maximum = 1.0f;                   // Slider
    float step = 0.1f;                      // Slider: one arrow press
    bool visible = true;
    bool enabled = true;
    bool blocks_pointer = false;            // Panel: hovering or clicking it gives it the pointer, so nothing behind it is hit
    std::function<void()> on_click;         // Button: runs once per activation
    std::function<void(float)> on_change;   // Slider: runs with the new value when it changes
};

// How the canvas maps to the viewport. The scale is min(width / reference_width, height / reference_height) * user_scale.
struct UiScaling {
    float reference_width = 1280.0f;
    float reference_height = 720.0f;
    float user_scale = 1.0f;
};

struct UiInputResult {
    // platform::kCapture* bits the UI owns after this batch: keyboard and gamepad while active, mouse while the pointer is over the UI
    // or a press is in progress. Pass them to InputRouter::set_capture(CaptureOwner::Ui, captured).
    std::uint8_t captured = 0;
    bool pointer_over_ui = false;
};

class Ui {
public:
    Ui();
    ~Ui();
    Ui(Ui&&) noexcept;
    Ui& operator=(Ui&&) noexcept;
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    void set_font(Font font);
    [[nodiscard]] bool has_font() const noexcept {
        return font_ != nullptr;
    }

    // Fails with InvalidArgument for a non-finite or non-positive size, and changes nothing.
    [[nodiscard]] core::Result<core::Unit, UiError> set_scaling(const UiScaling& scaling);
    [[nodiscard]] const UiScaling& scaling() const noexcept {
        return scaling_;
    }
    // The uniform scale from canvas units to pixels (1 before the first viewport).
    [[nodiscard]] float scale() const noexcept {
        return scale_;
    }
    [[nodiscard]] Rect canvas() const noexcept;

    void set_viewport(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] std::uint32_t viewport_width() const noexcept {
        return viewport_width_;
    }
    [[nodiscard]] std::uint32_t viewport_height() const noexcept {
        return viewport_height_;
    }

    // Registers a named image for Image elements. The name must be unused.
    [[nodiscard]] core::Result<core::Unit, UiError> add_image(std::string name, Surface image);

    // Adds an element under `parent` (kNoElement for the canvas). Only a Panel takes children.
    [[nodiscard]] core::Result<ElementId, UiError> add(ElementId parent, ElementSpec spec);
    // Removes the element and its subtree. Their ids stay unknown afterwards.
    [[nodiscard]] core::Result<core::Unit, UiError> remove(ElementId id);

    [[nodiscard]] core::Result<core::Unit, UiError> set_placement(ElementId id, const Placement& placement);
    [[nodiscard]] core::Result<core::Unit, UiError> set_label(ElementId id, std::string label);
    [[nodiscard]] core::Result<core::Unit, UiError> set_visible(ElementId id, bool visible);
    [[nodiscard]] core::Result<core::Unit, UiError> set_enabled(ElementId id, bool enabled);
    // Sets a slider's value, clamped to its range. Unlike a user change, this does not run on_change.
    [[nodiscard]] core::Result<core::Unit, UiError> set_value(ElementId id, float value);

    [[nodiscard]] core::Result<Rect, UiError> rect(ElementId id) const;
    [[nodiscard]] core::Result<float, UiError> value(ElementId id) const;
    // Whether the element exists and is visible, with every ancestor visible.
    [[nodiscard]] bool shown(ElementId id) const;

    // The focused element, or kNoElement. set_focus accepts only an enabled, shown Button or Slider (kNoElement clears focus).
    [[nodiscard]] ElementId focus() const noexcept {
        return focus_;
    }
    [[nodiscard]] core::Result<core::Unit, UiError> set_focus(ElementId id);

    // Inactive UI ignores input and captures nothing (a closed menu).
    void set_active(bool active) noexcept {
        active_ = active;
    }
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    // Pointer position in viewport pixels, clamped to the viewport.
    void set_pointer(float x, float y);
    [[nodiscard]] float pointer_x() const noexcept {
        return pointer_x_;
    }
    [[nodiscard]] float pointer_y() const noexcept {
        return pointer_y_;
    }
    // Pixels the pointer moves per device count of relative motion (default 1).
    void set_pointer_speed(float pixels_per_count) noexcept;

    // The topmost interactive element (enabled Button or Slider, or a blocking Panel) at a viewport pixel, or kNoElement.
    [[nodiscard]] ElementId hit_test(float x, float y) const;

    [[nodiscard]] UiInputResult handle(const std::vector<platform::InputEvent>& events);

    // The viewport-sized image of the tree, straight alpha.
    [[nodiscard]] Surface render() const;

private:
    struct Element;
    void relayout();
    void layout_element(ElementId id, const Rect& parent, bool parent_shown);
    void collect_focusable(ElementId id, std::vector<ElementId>& out) const;
    void collect_hits(ElementId id, float x, float y, ElementId& top) const;
    void move_focus(int step);
    void move_focus_spatial(int dx, int dy);
    void activate(ElementId id);
    void nudge_slider(ElementId id, float direction);
    void set_slider_from_pointer(ElementId id);
    void kill_subtree(ElementId id);
    void draw_element(Surface& target, ElementId id, PixelRect clip) const;
    void draw_label(Surface& target, PixelRect area, const std::string& label, Rgba8 colour, float text_size, TextAlign align,
                    PixelRect clip) const;
    [[nodiscard]] Element* find(ElementId id) noexcept;
    [[nodiscard]] const Element* find(ElementId id) const noexcept;
    [[nodiscard]] PixelRect to_pixels(const Rect& rect) const noexcept;

    std::vector<Element> elements_;  // element id is index + 1
    std::vector<ElementId> roots_;
    std::map<std::string, Surface> images_;
    std::shared_ptr<const Font> font_;
    UiScaling scaling_;
    float scale_ = 1.0f;
    std::uint32_t viewport_width_ = 0;
    std::uint32_t viewport_height_ = 0;
    float pointer_x_ = 0.0f;
    float pointer_y_ = 0.0f;
    float pointer_speed_ = 1.0f;
    ElementId focus_ = kNoElement;
    ElementId pressed_ = kNoElement;
    ElementId dragging_ = kNoElement;
    bool pointer_placed_ = false;
    bool active_ = false;
    bool shift_ = false;
};

}  // namespace engine::ui
