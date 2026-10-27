#pragma once

// Lua callbacks for the game UI (US-058). A Lua function on the Lua stack becomes a host callback, so a button's on_click or a
// slider's on_change can run script code. The callback holds a registry reference to the function; the last copy of the callback to
// go releases it. The engine must outlive every copy.
//
// A function that raises is reported to the engine's events (take_errors), and the host keeps running. A value that is not a
// function is reported too, and the returned callback does nothing.

#include "engine/script/script_engine.hpp"

#include <functional>

namespace engine::script {

// The callback for a Button: calls the Lua function with no arguments.
[[nodiscard]] std::function<void()> ui_action(ScriptEngine& engine, int index);

// The callback for a Slider: calls the Lua function with the new value.
[[nodiscard]] std::function<void(float)> ui_value_action(ScriptEngine& engine, int index);

}  // namespace engine::script
