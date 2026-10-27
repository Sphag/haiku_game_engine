#include "engine/editor/editor_model.hpp"

#include "engine/assets/asset_project.hpp"
#include "engine/scene/reflection.hpp"

#include <unordered_set>
#include <utility>
#include <variant>

namespace engine::editor {

namespace {

constexpr std::string_view kCategory = "editor";

}  // namespace

EditorModel::EditorModel(std::shared_ptr<Console> console, core::Path workspace_file)
    : console_(std::move(console)), workspace_file_(std::move(workspace_file)), scene_(console_), assets_(console_) {}

void EditorModel::startup(const std::string& project_override) {
    auto loaded = load_workspace(workspace_file_);
    if (loaded.ok()) {
        workspace_ = std::move(loaded).value();
    } else {
        note(core::LogLevel::Error, "workspace file " + workspace_file_.str() + " is not used: " + loaded.error().message);
    }

    const std::string target = project_override.empty() ? workspace_.last_project : project_override;
    if (target.empty()) {
        note(core::LogLevel::Info, "no project is open; open or create one to begin");
        return;
    }
    // open_project logs its own outcome, so a bad project only shows up in the Errors panel.
    static_cast<void>(open_project(target));
}

core::Result<core::Unit, ProjectError> EditorModel::open_project(std::string_view path_text) {
    stop();
    auto path = core::Path::from_utf8(path_text);
    if (!path.ok()) {
        ProjectError error{ProjectErrorCode::InvalidPath, "the project path is not valid UTF-8"};
        note(core::LogLevel::Error, error.message);
        return core::failure(std::move(error));
    }
    auto opened = project_.open(path.value());
    if (!opened.ok()) {
        note(core::LogLevel::Error, "cannot open project " + std::string(path_text) + ": " + opened.error().message);
        return opened;
    }
    workspace_.last_project = std::string(path_text);
    note(core::LogLevel::Info, "opened project " + project_.info()->name + " at " + std::string(path_text));
    if (project_.scan_error()) {
        note(core::LogLevel::Warning, "assets: " + project_.scan_error()->message);
    }
    load_project_prefabs();
    scene_.set_known_assets(known_assets());
    open_asset_browser();
    return core::success();
}

void EditorModel::load_project_prefabs() {
    // A scene names prefab instances by asset GUID, so every prefab the project holds is in the library before a scene opens.
    const assets::ProjectScan* scan = project_.scan();
    if (scan == nullptr || !project_.info()) {
        return;
    }
    const auto assets_root = project_.info()->root.join(std::string(kProjectAssetsFolder));
    if (!assets_root.ok()) {
        return;
    }
    for (const assets::AssetRecord* record : scan->registry.records()) {
        if (!record->source_path.str().ends_with(".prefab") || scene_.prefabs().find(record->guid) != nullptr) {
            continue;
        }
        const auto file = assets_root.value().join(record->source_path.str());
        const auto text = file.ok() ? core::read_text_file(file.value()) : core::IoResult<std::string>(core::failure(core::IoError{}));
        if (!text.ok()) {
            note(core::LogLevel::Warning, "prefab " + record->source_path.str() + " cannot be read");
            continue;
        }
        auto prefab = scene::parse_prefab(text.value(), scene_.catalog());
        if (!prefab.ok()) {
            note(core::LogLevel::Warning, "prefab " + record->source_path.str() + ": " + prefab.error().message);
            continue;
        }
        static_cast<void>(scene_.prefabs().add(record->guid, std::move(prefab).value()));
    }
}

gameplay::ScriptSource EditorModel::script_reader() const {
    // Script assets are read from the project's assets folder, so Play runs the file as it is on disk, saved or not in any other tool.
    return [this](const core::Guid& guid) -> core::Result<std::string, std::string> {
        const assets::ProjectScan* scan = project_.scan();
        const assets::AssetRecord* record = scan != nullptr ? scan->registry.find(guid) : nullptr;
        if (record == nullptr || !project_.info()) {
            return core::failure(std::string("not an asset of the open project"));
        }
        const auto file = project_.info()->root.join(std::string(kProjectAssetsFolder));
        const auto path = file.ok() ? file.value().join(record->source_path.str()) : file;
        auto text = path.ok() ? core::read_text_file(path.value()) : core::IoResult<std::string>(core::failure(core::IoError{}));
        if (!text.ok()) {
            return core::failure("cannot read " + record->source_path.str());
        }
        return core::success(std::move(text.value()));
    };
}

core::Result<core::Unit, ProjectError> EditorModel::create_project(std::string_view parent_text, std::string_view name) {
    stop();
    auto parent = core::Path::from_utf8(parent_text);
    if (!parent.ok()) {
        ProjectError error{ProjectErrorCode::InvalidPath, "the parent folder path is not valid UTF-8"};
        note(core::LogLevel::Error, error.message);
        return core::failure(std::move(error));
    }
    auto created = engine::editor::create_project(parent.value(), name);
    if (!created.ok()) {
        note(core::LogLevel::Error, "cannot create project " + std::string(name) + ": " + created.error().message);
        ProjectError error = created.error();
        return core::failure(std::move(error));
    }
    note(core::LogLevel::Info, "created project " + created.value().name + " at " + created.value().root.str());
    return open_project(created.value().root.str());
}

void EditorModel::close_project() {
    stop();
    if (project_.state() == ProjectState::Closed) {
        return;
    }
    assets_.close();
    project_.close();
    scene_.set_known_assets({});
    scene_.new_scene();
    note(core::LogLevel::Info, "closed the project");
}

void EditorModel::open_asset_browser() {
    assets_.close();
    if (project_.state() != ProjectState::Open) {
        return;
    }
    auto assets_root = project_.info()->root.join(std::string(kProjectAssetsFolder));
    if (!assets_root.ok()) {
        return;
    }
    const assets::ProjectScan empty;
    assets_.open(project_.info()->root, assets_root.value(), project_.scan() != nullptr ? *project_.scan() : empty);
}

void EditorModel::refresh_assets() {
    if (project_.state() != ProjectState::Open) {
        return;
    }
    project_.rescan();
    const assets::ProjectScan empty;
    assets_.refresh(project_.scan() != nullptr ? *project_.scan() : empty);
    scene_.set_known_assets(known_assets());
}

void EditorModel::tick_assets() {
    if (assets_.poll_changes()) {
        refresh_assets();
    }
    static_cast<void>(assets_.poll_import());
}

core::Result<core::Unit, std::string> EditorModel::import_assets() {
    return assets_.start_import();
}

core::Result<core::Unit, std::string> EditorModel::set_asset_settings(std::string_view guid, std::string_view settings_json) {
    auto saved = assets_.set_settings(guid, settings_json);
    refresh_assets();
    return saved;
}

core::Result<core::Unit, std::string> EditorModel::rename_asset(std::string_view guid, std::string_view new_source) {
    auto renamed = assets_.rename(guid, new_source);
    refresh_assets();
    return renamed;
}

core::Result<core::Unit, std::string> EditorModel::delete_asset(std::string_view guid) {
    auto removed = assets_.remove(guid);
    refresh_assets();
    return removed;
}

core::Result<core::Guid, std::string> EditorModel::place_asset(std::string_view guid) {
    const CatalogEntry* entry = assets_.find(guid);
    if (entry == nullptr || !entry->registered()) {
        std::string message = "only a registered asset can be placed; " + std::string(guid) + " is not one";
        note(core::LogLevel::Error, message);
        return core::failure(std::move(message));
    }
    const std::string field = entry->asset->importer;
    const core::Guid asset = entry->asset->guid;

    const scene::ComponentDescriptor* target = nullptr;
    for (const scene::ComponentDescriptor& component : scene_.catalog().components()) {
        const scene::FieldDescriptor* found = component.find_field(field);
        if (found != nullptr && found->kind() == scene::FieldKind::Guid) {
            target = &component;
            break;
        }
    }
    if (target == nullptr) {
        std::string message = "no component has a GUID field named '" + field + "', so " + entry->source + " cannot be placed";
        note(core::LogLevel::Error, message);
        return core::failure(std::move(message));
    }

    auto created = scene_.create_entity();
    if (!created.ok()) {
        std::string message = "cannot create an entity for " + entry->source + ": " + created.error().message;
        note(core::LogLevel::Error, message);
        return core::failure(std::move(message));
    }
    const core::Guid entity = created.value();
    auto attached = scene_.add_component(entity, target->type_id);
    if (!attached.ok()) {
        static_cast<void>(scene_.remove(entity));
        std::string message = "cannot place " + entry->source + ": " + attached.error().message;
        note(core::LogLevel::Error, message);
        return core::failure(std::move(message));
    }
    auto set = scene_.set_field(entity, target->type_id, field, scene::FieldValue(std::in_place_type<core::Guid>, asset));
    if (!set.ok()) {
        // The entity was made for this drop only, so it goes again on failure.
        static_cast<void>(scene_.remove(entity));
        std::string message = "cannot place " + entry->source + ": " + set.error().message;
        note(core::LogLevel::Error, message);
        return core::failure(std::move(message));
    }
    note(core::LogLevel::Info, "placed " + entry->source + " as " + target->type_id + "." + field);
    return core::success(entity);
}

std::unordered_set<core::Guid> EditorModel::known_assets() const {
    std::unordered_set<core::Guid> guids;
    if (const assets::ProjectScan* scan = project_.scan()) {
        for (const assets::AssetRecord* record : scan->registry.records()) {
            guids.insert(record->guid);
        }
    }
    return guids;
}

core::Result<core::Unit, WorkspaceError> EditorModel::save_workspace() {
    auto saved = engine::editor::save_workspace(workspace_file_, workspace_);
    if (!saved.ok()) {
        note(core::LogLevel::Error, "cannot save the workspace: " + saved.error().message);
        return saved;
    }
    note(core::LogLevel::Info, "saved the workspace to " + workspace_file_.str());
    return saved;
}

void EditorModel::reset_layout() {
    workspace_.layout.clear();
    layout_reset_ = true;
    note(core::LogLevel::Info, "the panel layout was reset");
}

bool EditorModel::take_layout_reset() noexcept {
    const bool requested = layout_reset_;
    layout_reset_ = false;
    return requested;
}

void EditorModel::set_panel_visible(Panel panel, bool visible) {
    switch (panel) {
        case Panel::Project:
            workspace_.panels.project = visible;
            return;
        case Panel::Console:
            workspace_.panels.console = visible;
            return;
        case Panel::Errors:
            workspace_.panels.errors = visible;
            return;
        case Panel::Viewport:
            workspace_.panels.viewport = visible;
            return;
        case Panel::Workspace:
            workspace_.panels.workspace = visible;
            return;
    }
}

bool EditorModel::panel_visible(Panel panel) const noexcept {
    switch (panel) {
        case Panel::Project:
            return workspace_.panels.project;
        case Panel::Console:
            return workspace_.panels.console;
        case Panel::Errors:
            return workspace_.panels.errors;
        case Panel::Viewport:
            return workspace_.panels.viewport;
        case Panel::Workspace:
            return workspace_.panels.workspace;
    }
    return false;
}

void EditorModel::update_viewport(const ViewportInput& input) {
    ViewportInput scene_input = input;
    if (!scene_editable()) {
        scene_input.primary_pressed = false;
        scene_input.primary_down = false;
    }
    if (std::optional<std::string> notice = viewport_.update(scene_, scene_input)) {
        note(core::LogLevel::Warning, *notice);
    }
}

PlayState EditorModel::play_state() const noexcept {
    if (play_ == nullptr) {
        return PlayState::Edit;
    }
    return play_->paused() ? PlayState::Paused : PlayState::Playing;
}

bool EditorModel::play() {
    if (play_ != nullptr) {
        return false;
    }
    auto created = PlaySession::create(scene_.catalog(), scene_.prefabs(), scene_.scene().to_document(), known_assets(), script_reader());
    if (!created.ok()) {
        note(core::LogLevel::Error, "cannot play the scene: " + created.error());
        return false;
    }
    play_ = std::move(created.value());
    note(core::LogLevel::Info, "play started");
    return true;
}

void EditorModel::toggle_pause() {
    if (play_ == nullptr) {
        return;
    }
    play_->set_paused(!play_->paused());
    note(core::LogLevel::Info, play_->paused() ? "paused" : "resumed");
}

void EditorModel::step() {
    if (play_ == nullptr || !play_->paused()) {
        return;
    }
    static_cast<void>(play_->step());
    report_play_errors();
}

void EditorModel::stop() {
    if (play_ == nullptr) {
        return;
    }
    report_play_errors();
    const std::uint64_t steps = play_->fixed_steps();
    play_.reset();
    note(core::LogLevel::Info, "play stopped after " + std::to_string(steps) + " fixed steps; the scene is as it was before Play");
}

void EditorModel::tick_play(double frame_seconds) {
    if (play_ == nullptr) {
        return;
    }
    play_->advance(frame_seconds);
    report_play_errors();
}

void EditorModel::report_play_errors() {
    if (play_ == nullptr) {
        return;
    }
    for (const std::string& message : play_->take_errors()) {
        note(core::LogLevel::Error, "play: " + message);
    }
}

void EditorModel::note(core::LogLevel level, std::string_view message) {
    console_->add(level, kCategory, message);
}

}  // namespace engine::editor
