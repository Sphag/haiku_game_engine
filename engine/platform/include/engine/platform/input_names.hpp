#pragma once

#include "engine/platform/input.hpp"

#include <optional>
#include <string_view>

namespace engine::platform {

// Stable names for input enums, as written in input profiles. A name is the enum identifier ("Space", "LeftX", "A").
// Unknown names return nullopt, and Key::Unknown has no name.
[[nodiscard]] std::string_view key_name(Key key) noexcept;
[[nodiscard]] std::optional<Key> key_from_name(std::string_view name) noexcept;

[[nodiscard]] std::string_view mouse_button_name(MouseButton button) noexcept;
[[nodiscard]] std::optional<MouseButton> mouse_button_from_name(std::string_view name) noexcept;

[[nodiscard]] std::string_view mouse_axis_name(MouseAxis axis) noexcept;
[[nodiscard]] std::optional<MouseAxis> mouse_axis_from_name(std::string_view name) noexcept;

[[nodiscard]] std::string_view gamepad_button_name(GamepadButton button) noexcept;
[[nodiscard]] std::optional<GamepadButton> gamepad_button_from_name(std::string_view name) noexcept;

[[nodiscard]] std::string_view gamepad_axis_name(GamepadAxis axis) noexcept;
[[nodiscard]] std::optional<GamepadAxis> gamepad_axis_from_name(std::string_view name) noexcept;

}  // namespace engine::platform
