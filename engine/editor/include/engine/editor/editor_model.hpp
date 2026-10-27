#pragma once

// The editor's document model (US-061): the open project, the saved workspace, the console, and the commands the UI runs. It has no
// ImGui or graphics dependency, so the project, workspace, and panel behaviour is tested without a window (docs/editor.md).
//
// Every command writes its outcome to the console, so a failed open or save is visible in the Errors panel and the editor keeps going.

#include "engine/core/path.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"
#include "engine/editor/asset_browser.hpp"
#include "engine/editor/console.hpp"
#include "engine/editor/play_session.hpp"
#include "engine/editor/project.hpp"
#include "engine/editor/scene_editor.hpp"
#include "engine/editor/viewport.hpp"
#include "engine/editor/workspace.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace engine::editor {

enum class Panel : std::uint8_t {
    Project,
    Console,
    Errors,
    Viewport,
    Workspace,
};

class EditorModel {
public:
    // The console receives every outcome of a command. The workspace file is read at startup and written by save_workspace().
    EditorModel(std::shared_ptr<Console> console, core::Path workspace_file);

    // Loads the workspace file, then opens `project_override` when it is set or else the last project. A failure is logged and
    // leaves the editor with no project, so startup never stops on a bad file.
    void startup(const std::string& project_override);

    // Opens the folder at path_text. A failure leaves the session Invalid with the reason; the last project is kept only on success.
    core::Result<core::Unit, ProjectError> open_project(std::string_view path_text);
    // Creates parent/name and opens it. The parent is a folder that already exists.
    core::Result<core::Unit, ProjectError> create_project(std::string_view parent_text, std::string_view name);
    // Closing or opening a project stops Play first, so a play world never outlives the project it was built from.
    void close_project();

    core::Result<core::Unit, WorkspaceError> save_workspace();

    // The docking layout is reset on the next frame; the UI calls take_layout_reset() to see it.
    void reset_layout();
    [[nodiscard]] bool take_layout_reset() noexcept;

    void set_panel_visible(Panel panel, bool visible);
    [[nodiscard]] bool panel_visible(Panel panel) const noexcept;

    void request_quit() noexcept {
        quit_requested_ = true;
    }
    [[nodiscard]] bool quit_requested() const noexcept {
        return quit_requested_;
    }

    [[nodiscard]] const ProjectSession& project() const noexcept {
        return project_;
    }
    [[nodiscard]] const WorkspaceSettings& workspace() const noexcept {
        return workspace_;
    }
    // The UI stores the docking layout here after the user arranges panels.
    [[nodiscard]] WorkspaceSettings& workspace() noexcept {
        return workspace_;
    }
    // The scene being edited. Its asset set follows the open project: a project that opens or closes updates it.
    [[nodiscard]] SceneEditor& scene() noexcept {
        return scene_;
    }
    [[nodiscard]] const SceneEditor& scene() const noexcept {
        return scene_;
    }
    // The scene viewport: the editor camera, the gizmo mode, and the drag in progress (viewport.hpp).
    [[nodiscard]] EditorViewport& viewport() noexcept {
        return viewport_;
    }
    [[nodiscard]] const EditorViewport& viewport() const noexcept {
        return viewport_;
    }
    // Runs one viewport frame against scene(). A notice from the viewport goes to the console as a warning. While the scene is
    // not in Edit, the viewport keeps its camera but gets no press, so it cannot pick or drag an entity of the edited scene.
    void update_viewport(const ViewportInput& input);

    // Play mode (US-065). Play copies the scene's document into a runtime world; the edited scene is not changed by it. Stop
    // destroys that world, so the editor shows the scene as it was before Play. Pause holds the clock and Step runs one fixed step
    // while paused. The host calls tick_play once per frame with the frame time.
    [[nodiscard]] PlayState play_state() const noexcept;
    // True when the scene can be edited. Play, Pause, and Step do not change it, but the scene is read-only while a session runs.
    [[nodiscard]] bool scene_editable() const noexcept {
        return play_state() == PlayState::Edit;
    }
    // Returns false when a session is already running, or when the scene does not load into a play world (logged).
    [[nodiscard]] bool play();
    // Switches between Playing and Paused. Does nothing in Edit.
    void toggle_pause();
    // Runs one fixed step while paused. Does nothing otherwise.
    void step();
    // Destroys the play world. Does nothing in Edit.
    void stop();
    void tick_play(double frame_seconds);
    [[nodiscard]] PlaySession* play_session() noexcept {
        return play_.get();
    }

    // The asset browser (US-066) follows the open project. The host calls tick_assets once per frame: it rescans after a change to the
    // assets folder and collects a finished import. Edits to assets (settings, rename, delete) rescan at once.
    [[nodiscard]] AssetBrowser& assets() noexcept {
        return assets_;
    }
    [[nodiscard]] const AssetBrowser& assets() const noexcept {
        return assets_;
    }
    void tick_assets();
    // Starts the cook of the open project's assets. Fails when no project is open or an import is already running (logged).
    [[nodiscard]] core::Result<core::Unit, std::string> import_assets();
    [[nodiscard]] core::Result<core::Unit, std::string> set_asset_settings(std::string_view guid, std::string_view settings_json);
    [[nodiscard]] core::Result<core::Unit, std::string> rename_asset(std::string_view guid, std::string_view new_source);
    [[nodiscard]] core::Result<core::Unit, std::string> delete_asset(std::string_view guid);
    // Drops a registered asset into the scene (US-066): a new entity under the root that holds the first catalog component with a
    // GUID field named after the asset's importer (for example "texture"). The field is set through the scene's checked edit. Returns the
    // new entity. Fails, with the reason logged, when no component has such a field.
    [[nodiscard]] core::Result<core::Guid, std::string> place_asset(std::string_view guid);

    // The window's DPI scale (1.0 is 96 DPI). The host sets it at startup and on a DPI change; the viewport sizes its handles by it.
    void set_dpi_scale(double scale) noexcept {
        dpi_scale_ = scale > 0.0 ? scale : 1.0;
    }
    [[nodiscard]] double dpi_scale() const noexcept {
        return dpi_scale_;
    }
    [[nodiscard]] Console& console() noexcept {
        return *console_;
    }
    [[nodiscard]] const Console& console() const noexcept {
        return *console_;
    }
    [[nodiscard]] const core::Path& workspace_file() const noexcept {
        return workspace_file_;
    }

private:
    void note(core::LogLevel level, std::string_view message);
    // Logs the script and physics failures a play session has raised since the last call.
    void report_play_errors();
    // The asset GUIDs of the open project's registry; empty when no project is open.
    [[nodiscard]] std::unordered_set<core::Guid> known_assets() const;
    // Points the asset browser at the open project, or closes it when none is open.
    void open_asset_browser();
    // Rescans the project and refreshes the browser and the scene's known assets.
    void refresh_assets();

    std::shared_ptr<Console> console_;
    core::Path workspace_file_;
    ProjectSession project_;
    SceneEditor scene_;
    EditorViewport viewport_;
    AssetBrowser assets_;
    WorkspaceSettings workspace_;
    double dpi_scale_ = 1.0;
    bool layout_reset_ = false;
    bool quit_requested_ = false;
    // The running play world, or null in Edit. Declared last, so it is destroyed before the scene and viewport it was built from.
    std::unique_ptr<PlaySession> play_;
};

}  // namespace engine::editor
