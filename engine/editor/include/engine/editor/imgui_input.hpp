#pragma once

// The in-house platform/input adapter for Dear ImGui (US-061). The platform layer delivers keys, text, buttons, and wheel notches as
// events; the pointer is absolute and polled, because MouseMoved only carries device counts. The adapter writes these into the
// current ImGui context's IO queue. It needs no graphics device, so a unit test drives it on a bare context.

#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"

#include <optional>

#include <imgui.h>

namespace engine::editor {

// The ImGui key for a platform key, or nullopt for a key the editor does not use.
[[nodiscard]] std::optional<ImGuiKey> to_imgui_key(platform::Key key) noexcept;

// Applies one event to the current context: keys, text, mouse buttons, and wheel notches. Relative motion is ignored, because the
// pointer is polled with set_imgui_pointer. Events for other windows must be filtered by the caller.
void handle_imgui_input(const platform::InputEvent& event);

// The pointer position in framebuffer pixels, or nullopt when the pointer is not over the window. Call once per frame.
void set_imgui_pointer(std::optional<platform::PointerPosition> position);
void set_imgui_focused(bool focused);

// The framebuffer size in pixels and the frame time in seconds. Call before ImGui::NewFrame.
void begin_imgui_frame(float display_width, float display_height, float delta_seconds);

}  // namespace engine::editor
