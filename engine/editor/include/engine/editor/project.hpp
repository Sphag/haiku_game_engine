#pragma once

// Editor projects (US-061). A project is a folder holding project.json (format engine.project, version 1, and a name) and an
// assets folder. The asset scan (engine/assets/asset_project.hpp) reads the assets folder. A project that fails to open is reported
// as Invalid with its error, and the editor stays usable; a missing or unreadable asset is listed, not fatal.
//
//   {
//     "format": "engine.project",
//     "version": 1,
//     "name": "Проект"
//   }

#include "engine/assets/asset_project.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr std::string_view kProjectFileName = "project.json";
inline constexpr std::string_view kProjectFormat = "engine.project";
inline constexpr int kProjectVersion = 1;
inline constexpr std::string_view kProjectAssetsFolder = "assets";
inline constexpr std::size_t kMaxProjectNameBytes = 128;

enum class ProjectErrorCode {
    Io,                  // the project file or folder could not be read or written
    Malformed,           // project.json is not a JSON object, or a key is missing, unknown, or has the wrong type
    WrongFormat,         // the format name is not engine.project
    UnsupportedVersion,  // the version is not kProjectVersion
    InvalidName,         // empty, too long, not UTF-8, a control character, a path separator, "." or "..", or a trailing dot or space
    InvalidPath,         // the path text is not valid UTF-8
    AlreadyExists,       // create: the target folder already exists
};

struct ProjectError {
    ProjectErrorCode code = ProjectErrorCode::Io;
    std::string message;  // UTF-8, for the console and the Errors panel
};

struct ProjectInfo {
    std::string name;
    core::Path root;
};

[[nodiscard]] std::string_view to_string(ProjectErrorCode code) noexcept;

// True for a name that is safe as a single folder name on both operating systems.
[[nodiscard]] bool is_valid_project_name(std::string_view name) noexcept;

// Reads and validates project.json under root. It does not scan the assets.
[[nodiscard]] core::Result<ProjectInfo, ProjectError> read_project(const core::Path& root);

// Creates parent/name with project.json and an empty assets folder. Fails with AlreadyExists when the folder exists, and writes
// nothing in that case or when a later step fails.
[[nodiscard]] core::Result<ProjectInfo, ProjectError> create_project(const core::Path& parent, std::string_view name);

enum class ProjectState {
    Closed,   // no project is open
    Open,     // project.json is valid; the assets were scanned (scan_error says whether the scan found a problem)
    Invalid,  // the last open failed; error() says why
};

// The open project and its asset scan. Opening replaces the previous project, and a failed open leaves the session Invalid.
class ProjectSession {
public:
    core::Result<core::Unit, ProjectError> open(const core::Path& root);
    void close();
    // Scans the assets folder again (hot reload, and after an edit to the assets). Does nothing unless the project is Open.
    void rescan();

    [[nodiscard]] ProjectState state() const noexcept {
        return state_;
    }
    [[nodiscard]] const std::optional<ProjectInfo>& info() const noexcept {
        return info_;
    }
    [[nodiscard]] const std::optional<ProjectError>& error() const noexcept {
        return error_;
    }
    // The asset scan of an Open project; null otherwise.
    [[nodiscard]] const assets::ProjectScan* scan() const noexcept {
        return scan_ ? &*scan_ : nullptr;
    }
    // Why the assets folder could not be scanned. The project is still Open; the Project panel shows it.
    [[nodiscard]] const std::optional<assets::ScanError>& scan_error() const noexcept {
        return scan_error_;
    }

private:
    ProjectState state_ = ProjectState::Closed;
    std::optional<ProjectInfo> info_;
    std::optional<ProjectError> error_;
    std::optional<assets::ProjectScan> scan_;
    std::optional<assets::ScanError> scan_error_;
};

}  // namespace engine::editor
