#pragma once

// A UI layout file (engine.ui_layout, version 1): the elements of a screen as data, so a HUD or a menu is an asset and not code. The file is
// JSON with a flat list of elements, drawn in order:
//
//   { "format": "engine.ui_layout", "version": 1, "reference": [1280, 720],
//     "elements": [
//       { "kind": "panel",  "anchor": "TopLeft", "x": 16, "y": 16, "w": 280, "h": 96, "color": [0, 0, 0, 150] },
//       { "kind": "text",   "anchor": "TopLeft", "x": 28, "y": 24, "w": 256, "h": 32, "label": "Showcase",
//         "text_color": [255, 255, 255, 255], "text_size": 26, "align": "start" },
//       { "kind": "button", "anchor": "TopLeft", "x": 28, "y": 64, "w": 120, "h": 36, "label": "Reset", "on_click": "on_reset" },
//       { "kind": "slider", "anchor": "TopLeft", "x": 160, "y": 70, "w": 120, "h": 24, "value": 0.8, "min": 0, "max": 1,
//         "on_change": "on_volume" } ] }
//
// Kinds: panel, text, button, slider, image. Anchors are the nine Anchor names. Colors are [r, g, b, a] in 0..255. `on_click` and
// `on_change` name Lua functions in the canvas's script; the host binds them. An unknown key, kind, or anchor is an error, so a typo is
// never a silent default.

#include "engine/core/result.hpp"
#include "engine/ui/ui.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace engine::ui {

struct LayoutElement {
    ElementSpec spec;          // the element, without callbacks
    std::string on_click;      // the name of the Lua function a button runs, or empty
    std::string on_change;     // the name of the Lua function a slider runs with its value, or empty
};

struct UiLayout {
    float reference_width = 1280.0f;
    float reference_height = 720.0f;
    std::vector<LayoutElement> elements;
};

[[nodiscard]] core::Result<UiLayout, std::string> parse_ui_layout(std::string_view text);

}  // namespace engine::ui
