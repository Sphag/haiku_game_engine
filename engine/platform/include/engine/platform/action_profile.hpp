#pragma once

// The saved user profile for actions (US-011): the user's binding overrides, written as JSON. A profile carries a format
// name and a version. A file with another format or version is rejected with an error, never guessed at.
//
//   {
//     "format": "engine.input_profile",
//     "version": 1,
//     "actions": {
//       "jump": [ { "source": "key", "key": "Space", "scale": 1.0 } ],
//       "move_x": [
//         { "source": "key", "key": "D", "scale": 1.0 },
//         { "source": "key", "key": "A", "scale": -1.0 },
//         { "source": "gamepad_axis", "axis": "LeftX", "slot": 0, "scale": 1.0, "dead_zone": 0.2 }
//       ]
//     }
//   }
//
// Sources: "key", "mouse_button", "mouse_axis", "gamepad_button", "gamepad_axis". Names are the enum identifiers from
// input_names.hpp. "slot" is optional and omitted when the binding reads any gamepad. "scale" defaults to 1 and
// "dead_zone" to 0.

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/platform/action_map.hpp"

#include <string>
#include <string_view>

namespace engine::platform {

inline constexpr std::string_view kInputProfileFormat = "engine.input_profile";
inline constexpr int kInputProfileVersion = 1;

// Text of a profile. Output is deterministic: actions are sorted by name.
[[nodiscard]] std::string serialize_profile(const ProfileBindings& profile);

// Reads profile text. Checks the format, the version, the JSON structure, and every source name. It does not check the
// bindings' values; ActionMap::apply_overrides does that, with the action name in the error.
[[nodiscard]] core::Result<ProfileBindings, ActionError> parse_profile(std::string_view text);

// Writes the profile atomically (see core::write_file_atomic). The parent directory must exist.
[[nodiscard]] core::Result<core::Unit, ActionError> save_profile(const core::Path& path, const ProfileBindings& profile);

// Reads and parses a profile file. A missing or unreadable file is an Io error.
[[nodiscard]] core::Result<ProfileBindings, ActionError> load_profile(const core::Path& path);

}  // namespace engine::platform
