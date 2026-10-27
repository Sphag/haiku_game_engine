#include "engine/editor/project.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/utf8.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace engine::editor {

namespace {

ProjectError make_error(ProjectErrorCode code, std::string message) {
    return ProjectError{code, std::move(message)};
}

bool is_forbidden_name_char(unsigned char c) {
    // Control characters and the characters Windows rejects in file names.
    return c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
}

// project.json has exactly these three keys. An unknown key is an error, so a newer file is never read as if it were version 1.
bool has_exactly_keys(const nlohmann::json& object) {
    if (object.size() != 3) {
        return false;
    }
    return object.contains("format") && object.contains("version") && object.contains("name");
}

}  // namespace

std::string_view to_string(ProjectErrorCode code) noexcept {
    switch (code) {
        case ProjectErrorCode::Io:
            return "Io";
        case ProjectErrorCode::Malformed:
            return "Malformed";
        case ProjectErrorCode::WrongFormat:
            return "WrongFormat";
        case ProjectErrorCode::UnsupportedVersion:
            return "UnsupportedVersion";
        case ProjectErrorCode::InvalidName:
            return "InvalidName";
        case ProjectErrorCode::InvalidPath:
            return "InvalidPath";
        case ProjectErrorCode::AlreadyExists:
            return "AlreadyExists";
    }
    return "Unknown";
}

bool is_valid_project_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxProjectNameBytes || !core::is_valid_utf8(name)) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        if (is_forbidden_name_char(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    // Windows silently drops a trailing dot or space, so two names would name one folder.
    return name.back() != '.' && name.back() != ' ';
}

core::Result<ProjectInfo, ProjectError> read_project(const core::Path& root) {
    const auto file_path = root.join(kProjectFileName);
    if (!file_path.ok()) {
        return core::failure(make_error(ProjectErrorCode::InvalidPath, "project path: " + std::string(core::to_string(file_path.error()))));
    }
    const auto text = core::read_file(file_path.value());
    if (!text.ok()) {
        return core::failure(make_error(ProjectErrorCode::Io, core::describe(text.error())));
    }

    const auto document = nlohmann::json::parse(text.value(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return core::failure(make_error(ProjectErrorCode::Malformed, "project.json is not a JSON object"));
    }
    if (!has_exactly_keys(document)) {
        return core::failure(make_error(ProjectErrorCode::Malformed, "project.json needs exactly the keys format, version, and name"));
    }
    if (!document["format"].is_string() || document["format"].get<std::string>() != kProjectFormat) {
        return core::failure(make_error(ProjectErrorCode::WrongFormat, "project.json is not an engine.project file"));
    }
    if (!document["version"].is_number_integer()) {
        return core::failure(make_error(ProjectErrorCode::Malformed, "project.json version is not an integer"));
    }
    if (document["version"].get<long long>() != kProjectVersion) {
        return core::failure(make_error(ProjectErrorCode::UnsupportedVersion, "project.json has an unsupported version"));
    }
    if (!document["name"].is_string()) {
        return core::failure(make_error(ProjectErrorCode::Malformed, "project.json name is not a string"));
    }
    std::string name = document["name"].get<std::string>();
    if (!is_valid_project_name(name)) {
        return core::failure(make_error(ProjectErrorCode::InvalidName, "project.json name is not a valid project name"));
    }
    return core::success(ProjectInfo{std::move(name), root});
}

core::Result<ProjectInfo, ProjectError> create_project(const core::Path& parent, std::string_view name) {
    if (!is_valid_project_name(name)) {
        return core::failure(make_error(ProjectErrorCode::InvalidName, "a project name must be 1 to 128 UTF-8 bytes, with no path separators"));
    }
    if (!core::path_exists(parent)) {
        return core::failure(make_error(ProjectErrorCode::Io, "the parent folder does not exist: " + parent.str()));
    }
    const auto root = parent.join(name);
    if (!root.ok()) {
        return core::failure(make_error(ProjectErrorCode::InvalidPath, "project path: " + std::string(core::to_string(root.error()))));
    }
    if (core::path_exists(root.value())) {
        return core::failure(make_error(ProjectErrorCode::AlreadyExists, "a folder with this name already exists: " + root.value().str()));
    }

    // Each step may fail; a failure removes the folder this call created, so no half-made project is left behind.
    const auto assets = root.value().join(kProjectAssetsFolder);
    if (!assets.ok()) {
        return core::failure(make_error(ProjectErrorCode::InvalidPath, "assets path: " + std::string(core::to_string(assets.error()))));
    }
    if (const auto made = core::create_directories(assets.value()); !made.ok()) {
        static_cast<void>(core::remove_all(root.value()));
        return core::failure(make_error(ProjectErrorCode::Io, core::describe(made.error())));
    }

    nlohmann::json document;
    document["format"] = std::string(kProjectFormat);
    document["version"] = kProjectVersion;
    document["name"] = std::string(name);
    const std::string text = document.dump(2, ' ', false, nlohmann::json::error_handler_t::strict) + "\n";
    const auto file_path = root.value().join(kProjectFileName);
    if (!file_path.ok()) {
        static_cast<void>(core::remove_all(root.value()));
        return core::failure(make_error(ProjectErrorCode::InvalidPath, "project path: " + std::string(core::to_string(file_path.error()))));
    }
    if (const auto written = core::write_file_atomic(file_path.value(), text); !written.ok()) {
        static_cast<void>(core::remove_all(root.value()));
        return core::failure(make_error(ProjectErrorCode::Io, core::describe(written.error())));
    }
    return core::success(ProjectInfo{std::string(name), root.value()});
}

core::Result<core::Unit, ProjectError> ProjectSession::open(const core::Path& root) {
    close();
    auto info = read_project(root);
    if (!info.ok()) {
        state_ = ProjectState::Invalid;
        error_ = info.error();
        return core::failure(info.error());
    }
    info_ = std::move(info).value();

    const auto assets_path = info_->root.join(kProjectAssetsFolder);
    if (!assets_path.ok()) {
        close();
        state_ = ProjectState::Invalid;
        error_ = make_error(ProjectErrorCode::InvalidPath, "assets path: " + std::string(core::to_string(assets_path.error())));
        return core::failure(*error_);
    }
    state_ = ProjectState::Open;
    rescan();
    return core::success();
}

void ProjectSession::rescan() {
    if (state_ != ProjectState::Open || !info_) {
        return;
    }
    scan_.reset();
    scan_error_.reset();
    auto assets_path = info_->root.join(kProjectAssetsFolder);
    if (!assets_path.ok()) {
        return;
    }
    auto scan = assets::scan_project(assets_path.value());
    if (scan.ok()) {
        scan_ = std::move(scan).value();
    } else {
        // A project without an assets folder is still open; the Project panel says what is missing.
        scan_error_ = scan.error();
        scan_ = assets::ProjectScan{};
    }
}

void ProjectSession::close() {
    state_ = ProjectState::Closed;
    info_.reset();
    error_.reset();
    scan_.reset();
    scan_error_.reset();
}

}  // namespace engine::editor
