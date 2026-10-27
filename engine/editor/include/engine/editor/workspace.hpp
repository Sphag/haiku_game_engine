#pragma once

// Saved editor workspace (US-061): the last project, the panel visibility, and the docking layout. The layout is the text ImGui
// writes with SaveIniSettingsToMemory, stored as one JSON string, so a workspace file is the only file the editor keeps.
//
//   {
//     "format": "engine.editor_workspace",
//     "version": 1,
//     "last_project": "D:/games/demo",
//     "layout": "[Window][Console]\nPos=...",
//     "panels": { "project": true, "console": true, "errors": true, "viewport": true, "workspace": false }
//   }
//
// A file with an unknown key, a wrong type, or another format or version is an error, and the host falls back to the defaults and
// logs the reason. A missing file is not an error: it means the defaults.

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr std::string_view kWorkspaceFormat = "engine.editor_workspace";
inline constexpr int kWorkspaceVersion = 1;

struct PanelVisibility {
    bool project = true;
    bool console = true;
    bool errors = true;
    bool viewport = true;
    bool workspace = false;

    friend bool operator==(const PanelVisibility& lhs, const PanelVisibility& rhs) noexcept {
        return lhs.project == rhs.project && lhs.console == rhs.console && lhs.errors == rhs.errors && lhs.viewport == rhs.viewport &&
               lhs.workspace == rhs.workspace;
    }
    friend bool operator!=(const PanelVisibility& lhs, const PanelVisibility& rhs) noexcept {
        return !(lhs == rhs);
    }
};

struct WorkspaceSettings {
    std::string last_project;  // the project folder's path text, or empty for none
    std::string layout;        // ImGui ini text of the docking layout, or empty for the default layout
    PanelVisibility panels;

    friend bool operator==(const WorkspaceSettings& lhs, const WorkspaceSettings& rhs) noexcept {
        return lhs.last_project == rhs.last_project && lhs.layout == rhs.layout && lhs.panels == rhs.panels;
    }
    friend bool operator!=(const WorkspaceSettings& lhs, const WorkspaceSettings& rhs) noexcept {
        return !(lhs == rhs);
    }
};

enum class WorkspaceErrorCode {
    Io,                  // the file could not be read or written
    Malformed,           // not a JSON object, a key is missing or unknown, or a value has the wrong type
    WrongFormat,         // the format name is not engine.editor_workspace
    UnsupportedVersion,  // the version is not kWorkspaceVersion
};

struct WorkspaceError {
    WorkspaceErrorCode code = WorkspaceErrorCode::Io;
    std::string message;  // UTF-8, for the console
};

[[nodiscard]] std::string_view to_string(WorkspaceErrorCode code) noexcept;

// Parses the workspace text. Strict: the text must hold exactly the keys above.
[[nodiscard]] core::Result<WorkspaceSettings, WorkspaceError> parse_workspace(std::string_view text);
[[nodiscard]] std::string serialize_workspace(const WorkspaceSettings& settings);

// A missing file loads as the defaults. Any other read or parse failure is an error.
[[nodiscard]] core::Result<WorkspaceSettings, WorkspaceError> load_workspace(const core::Path& file);
// Writes through write_file_atomic, so a failed save leaves the previous workspace in place.
[[nodiscard]] core::Result<core::Unit, WorkspaceError> save_workspace(const core::Path& file, const WorkspaceSettings& settings);

}  // namespace engine::editor
