# Game UI (US-058)

The game UI lives in `engine/ui/`. It is a CPU library: layout, text, input, and a raster of the tree. It does not need the editor, ImGui,
or a GPU. `engine/script/include/engine/script/script_ui.hpp` connects Lua functions to its callbacks.

## Layout and scaling

- The canvas is the viewport divided by the UI scale. The scale is `min(width / reference_width, height / reference_height) * user_scale`
  (`UiScaling`, default reference 1280 x 720). Layouts are written in canvas units and read the same at every window size.
- An element's `Placement` is an anchor box, given as fractions of its parent, plus offsets. `place(Anchor, x, y, w, h)` is a fixed-size box
  whose top left is the anchor point plus (x, y); `stretch(margins)` fills the parent inside margins.
- Elements are `Panel` (the only container), `Image` (a named image registered with `add_image`), `Text`, `Button`, and `Slider`.
  `visible` hides an element and its subtree. Disabled elements are drawn but do not take input.

## Text

- The font is a TrueType or OpenType file read by the host (`Font::load`). The fixtures use the vendored Roboto Regular, which covers
  Cyrillic. The C shim `engine/ui/src/native/truetype_shim.c` wraps the vendored stb_truetype.
- Strings are UTF-8. Invalid or truncated sequences draw U+FFFD. Text is one line, aligned start, centre, or end in its rectangle.

## Input

- `Ui::handle` takes the platform's input events. Pointer motion is device counts (`MouseMoved`) added to a pointer that starts at the
  viewport centre and is clamped to it. `set_pointer` places it from an absolute cursor.
- Tab and Shift+Tab cycle focus in tree order over enabled buttons and sliders. The arrow keys and the D-pad move to the nearest element
  in that direction. Enter, Space, or gamepad A activates the focused button. Left and Right change the focused slider by its step.
- A click activates a button when the press and release are over the same button. Dragging sets a slider.
- `set_active(true)` turns input on (an open menu). An inactive UI consumes nothing and captures nothing.
- `handle` returns the `platform::kCapture*` bits the UI owns. The host passes them to
  `InputRouter::set_capture(CaptureOwner::Ui, bits)`, so gameplay does not see keys, pad buttons, or (while the pointer is over the UI) the
  mouse.

## Callbacks

- A `Button` runs `on_click`. A `Slider` runs `on_change` with the new value on a user change, not on `set_value`.
- From Lua, `script::ui_action(engine, index)` and `script::ui_value_action(engine, index)` turn a function on the Lua stack into those
  callbacks. A raised error goes to `ScriptEngine::take_errors()`, and the host keeps running. The registry reference is released with the
  last copy of the callback, so the `ScriptEngine` must outlive the UI.

## Composition

- `Ui::render()` returns a viewport-sized RGBA8 surface with straight alpha, the layout of `ImageRgba8`. The host composites it after
  scene post-processing with `PostProcessor::set_overlay`, which blends it in display space after tone mapping and the sRGB encode.
  Call `set_overlay` before `run`.

## Tests

- `tests/src/ui_layout_tests.cpp`: anchors, scale, and the corner of an anchored element at 640x360, 800x600, 1280x720, 1920x1080, and
  2560x1080.
- `tests/src/ui_text_tests.cpp`: UTF-8 decoding, the font, Cyrillic measurement and drawing, and a button at three window sizes.
- `tests/src/ui_interaction_tests.cpp`: hit testing, focus, navigation, clicks, sliders, capture, removal, and images.
- `tests/src/script_ui_tests.cpp`: Lua callbacks through the real VM, including error reporting and stack balance.

These run on every OS with no window. Visual checks in a running application are not part of this change: the repository has no
application host that calls the overlay yet.
