#include "engine/editor/workspace.hpp"

#include "engine/core/file_io.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace engine::editor {

namespace {

WorkspaceError make_error(WorkspaceErrorCode code, std::string message) {
    return WorkspaceError{code, std::move(message)};
}

bool has_exactly_keys(const nlohmann::json& object, std::initializer_list<const char*> keys) {
    if (object.size() != keys.size()) {
        return false;
    }
    for (const char* key : keys) {
        if (!object.contains(key)) {
            return false;
        }
    }
    return true;
}

// Reads one panel flag. A missing or non-boolean value is an error, so a typo never silently hides a panel.
core::Result<bool, WorkspaceError> panel_flag(const nlohmann::json& panels, const char* key) {
    if (!panels[key].is_boolean()) {
        return core::failure(make_error(WorkspaceErrorCode::Malformed, std::string("panels.") + key + " is not a boolean"));
    }
    return core::success(panels[key].get<bool>());
}

}  // namespace

std::string_view to_string(WorkspaceErrorCode code) noexcept {
    switch (code) {
        case WorkspaceErrorCode::Io:
            return "Io";
        case WorkspaceErrorCode::Malformed:
            return "Malformed";
        case WorkspaceErrorCode::WrongFormat:
            return "WrongFormat";
        case WorkspaceErrorCode::UnsupportedVersion:
            return "UnsupportedVersion";
    }
    return "Unknown";
}

core::Result<WorkspaceSettings, WorkspaceError> parse_workspace(std::string_view text) {
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return core::failure(make_error(WorkspaceErrorCode::Malformed, "the workspace file is not a JSON object"));
    }
    // recent_projects is optional, so a workspace written before it existed still loads.
    nlohmann::json required = document;
    required.erase("recent_projects");
    if (!has_exactly_keys(required, {"format", "version", "last_project", "layout", "panels"})) {
        return core::failure(make_error(WorkspaceErrorCode::Malformed, "the workspace file needs exactly the keys of engine.editor_workspace"));
    }
    if (!document["format"].is_string() || document["format"].get<std::string>() != kWorkspaceFormat) {
        return core::failure(make_error(WorkspaceErrorCode::WrongFormat, "the file is not an engine.editor_workspace file"));
    }
    if (!document["version"].is_number_integer()) {
        return core::failure(make_error(WorkspaceErrorCode::Malformed, "the workspace version is not an integer"));
    }
    if (document["version"].get<long long>() != kWorkspaceVersion) {
        return core::failure(make_error(WorkspaceErrorCode::UnsupportedVersion, "the workspace file has an unsupported version"));
    }
    if (!document["last_project"].is_string() || !document["layout"].is_string()) {
        return core::failure(make_error(WorkspaceErrorCode::Malformed, "last_project and layout must be strings"));
    }

    const auto& panels = document["panels"];
    if (!panels.is_object() || !has_exactly_keys(panels, {"project", "console", "errors", "viewport", "workspace"})) {
        return core::failure(make_error(WorkspaceErrorCode::Malformed, "panels must hold exactly the five panel flags"));
    }

    WorkspaceSettings settings;
    if (document.contains("recent_projects")) {
        const auto& recent = document["recent_projects"];
        if (!recent.is_array() || recent.size() > kMaxRecentProjects) {
            return core::failure(make_error(WorkspaceErrorCode::Malformed, "recent_projects must be an array of at most 8 strings"));
        }
        for (const auto& item : recent) {
            if (!item.is_string()) {
                return core::failure(make_error(WorkspaceErrorCode::Malformed, "recent_projects must hold strings"));
            }
            settings.recent_projects.push_back(item.get<std::string>());
        }
    }
    settings.last_project = document["last_project"].get<std::string>();
    settings.layout = document["layout"].get<std::string>();
    // Each flag is read in turn, so the first bad one names itself in the error.
    const auto project = panel_flag(panels, "project");
    if (!project.ok()) {
        return core::failure(project.error());
    }
    settings.panels.project = project.value();
    const auto console = panel_flag(panels, "console");
    if (!console.ok()) {
        return core::failure(console.error());
    }
    settings.panels.console = console.value();
    const auto errors = panel_flag(panels, "errors");
    if (!errors.ok()) {
        return core::failure(errors.error());
    }
    settings.panels.errors = errors.value();
    const auto viewport = panel_flag(panels, "viewport");
    if (!viewport.ok()) {
        return core::failure(viewport.error());
    }
    settings.panels.viewport = viewport.value();
    const auto workspace = panel_flag(panels, "workspace");
    if (!workspace.ok()) {
        return core::failure(workspace.error());
    }
    settings.panels.workspace = workspace.value();
    return core::success(std::move(settings));
}

std::string serialize_workspace(const WorkspaceSettings& settings) {
    nlohmann::json panels;
    panels["project"] = settings.panels.project;
    panels["console"] = settings.panels.console;
    panels["errors"] = settings.panels.errors;
    panels["viewport"] = settings.panels.viewport;
    panels["workspace"] = settings.panels.workspace;

    nlohmann::json document;
    document["format"] = std::string(kWorkspaceFormat);
    document["version"] = kWorkspaceVersion;
    document["last_project"] = settings.last_project;
    document["layout"] = settings.layout;
    if (!settings.recent_projects.empty()) {
        document["recent_projects"] = settings.recent_projects;
    }
    document["panels"] = std::move(panels);
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::strict) + "\n";
}

core::Result<WorkspaceSettings, WorkspaceError> load_workspace(const core::Path& file) {
    if (!core::path_exists(file)) {
        return core::success(WorkspaceSettings{});
    }
    const auto text = core::read_file(file);
    if (!text.ok()) {
        return core::failure(make_error(WorkspaceErrorCode::Io, core::describe(text.error())));
    }
    return parse_workspace(text.value());
}

core::Result<core::Unit, WorkspaceError> save_workspace(const core::Path& file, const WorkspaceSettings& settings) {
    const auto written = core::write_file_atomic(file, serialize_workspace(settings));
    if (!written.ok()) {
        return core::failure(make_error(WorkspaceErrorCode::Io, core::describe(written.error())));
    }
    return core::success();
}

}  // namespace engine::editor
