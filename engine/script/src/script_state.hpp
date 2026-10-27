#pragma once

// Plain-data state of one script instance (US-059), internal to engine_script. The encoder reads the instance's environment table
// and writes a JSON object; the decoder stages the object into a new table and then copies it into the environment, so a bad value
// leaves the environment unchanged. Functions, userdata, and threads are skipped: callbacks come from the chunk, and host handles
// are not state.

#include "engine/core/result.hpp"
#include "engine/script/script_error.hpp"

#include <string>
#include <string_view>

struct lua_State;

namespace engine::script::detail {

[[nodiscard]] core::Result<std::string, ScriptError> encode_environment(lua_State* state, int env_ref);
[[nodiscard]] core::Result<core::Unit, ScriptError> decode_environment(lua_State* state, int env_ref, std::string_view text);

}  // namespace engine::script::detail
