#pragma once

// The editor shell's frame (US-061) and the scene panels (US-062). The menu bar, a docking space with a default layout, and the Project,
// Console, Errors, Viewport, and Workspace panels come from US-061. The Hierarchy and Inspector panels are always shown: they edit the
// scene in EditorModel::scene(). The UI reads and changes the model and keeps only its text fields and the status of its last command.
// The Viewport panel (US-063) draws the scene's markers and the gizmo, and reads the pointer and keys into EditorModel::viewport().
//
// Destructive actions (a new scene, opening a scene, closing the project, and exiting) ask first when the scene has unsaved changes.
//
// The Assets panel (US-066) browses the project's catalog. An asset dragged from it onto an inspector Guid field sets the reference, and one
// dropped on the viewport is placed as a new entity (EditorModel::place_asset).

#include "engine/editor/distribution.hpp"
#include "engine/editor/editor_model.hpp"
#include "engine/editor/animation_graph_panel.hpp"
#include "engine/editor/material_graph_panel.hpp"
#include "engine/editor/terrain_panel.hpp"
#include "engine/editor/world_panel.hpp"

#include <array>
#include <string>
#include <vector>

namespace engine::editor {

class EditorUi {
public:
    // Takes the model's workspace into account: a saved layout is applied with apply_saved_layout(), and an empty one uses the default.
    explicit EditorUi(EditorModel& model);

    // Loads the saved docking layout into ImGui. Call once after the context is created and before the first build().
    void apply_saved_layout();
    // Stores the current docking layout in the workspace, so save_workspace() writes it.
    void capture_layout();

    // Asks to exit. The window's close button calls this, so unsaved scene changes get the dialog first.
    void request_exit() noexcept {
        exit_requested_ = true;
    }

    // Builds one frame between ImGui::NewFrame and ImGui::Render. The context must have ImGuiConfigFlags_DockingEnable set; the
    // docking calls create no node without it.
    void build();

private:
    // A command that discards the scene's unsaved changes. It runs at once when the scene is clean, else after the dialog.
    enum class PendingAction { None, NewScene, OpenScene, CloseProject, Exit };

    void dock_space();
    void history_shortcuts();
    // Opens a scene asset of the project (a path relative to its assets folder), asking about unsaved changes first.
    void open_scene_asset(const std::string& relative);
    void editor_shortcuts();
    void menu_bar();
    void project_panel();
    void console_panel();
    void errors_panel();
    void material_graph_window();
    void animation_graph_window();
    void terrain_window();
    void world_window();
    void build_game_window();
    void viewport_panel();
    void workspace_panel();
    void assets_panel();
    void assets_body();
    void assets_detail(const CatalogEntry& entry);
    void asset_action_dialog();
    void open_asset_action(bool rename, const std::string& guid, const std::string& target);
    void hierarchy_panel();
    void inspector_panel();
    void transform_section(const core::Guid& entity, bool read_only);
    void component_section(const core::Guid& entity, bool read_only);
    void field_editor(const core::Guid& entity, const scene::ComponentDescriptor& descriptor, const scene::FieldDescriptor& field);
    void guid_field(const core::Guid& entity, const scene::ComponentDescriptor& descriptor, const scene::FieldDescriptor& field,
                    const core::Guid& value);
    // Sets a Guid field of an entity; a failure goes to the inspector's status line.
    void set_guid_field(const core::Guid& entity, const scene::ComponentDescriptor& descriptor, const scene::FieldDescriptor& field,
                        const core::Guid& value);
    void add_component_row(const core::Guid& entity, bool read_only);
    void unsaved_dialog();
    void request(PendingAction action);
    void perform(PendingAction action);
    bool save_scene();
    void open_scene();
    void default_layout(unsigned int dock_id);

    EditorModel& model_;
    bool default_layout_needed_ = false;
    bool exit_requested_ = false;
    bool material_graph_open_ = false;       // the material graph window; not saved in the workspace
    MaterialGraphPanel material_graph_;
    bool animation_graph_open_ = false;      // the animation graph window; not saved in the workspace
    AnimationGraphPanel animation_graph_;
    bool terrain_open_ = false;              // the terrain window; not saved in the workspace
    TerrainPanel terrain_;
    bool world_open_ = false;                // the world window; not saved in the workspace
    WorldPanel world_;
    bool build_open_ = false;                // the Build Game window; not saved in the workspace
    GameBuilder game_build_;                 // runs the build on a worker
    std::array<char, 1024> build_output_{};  // the folder that receives the package
    std::array<char, 80> build_name_{};      // the package folder name
    std::array<char, 40> build_scene_{};     // the start scene GUID
    int build_backend_ = 0;                  // 0 dx12, 1 vulkan
    std::string build_status_;               // the last build's outcome, or empty
    bool open_dialog_ = false;
    PendingAction pending_ = PendingAction::None;
    std::array<char, 1024> path_text_{};     // the project folder to open
    std::array<char, 1024> parent_text_{};   // the parent folder for a new project
    std::array<char, 256> name_text_{};      // the new project's name
    std::array<char, 1024> scene_text_{};    // the scene file to open or save
    std::array<char, 128> search_text_{};    // the hierarchy filter
    std::array<char, 40> guid_text_{};       // the text of the Guid field being edited
    std::string guid_key_;                   // which Guid field guid_text_ belongs to
    std::string scene_status_;               // the last scene command's error, or empty
    std::string dialog_status_;              // the last error in the unsaved-changes dialog
    int add_component_index_ = 0;            // the Add component combo's choice
    int level_filter_ = 0;                   // Console: 0 all, 1 info and up, 2 warnings and up, 3 errors only
    bool orbit_captured_ = false;            // a right-button press began over the viewport and is still held
    bool pan_captured_ = false;              // a middle-button press began over the viewport and is still held
    bool inspector_gesture_ = false;         // an inspector item is active, so its edits merge into one undo step
    std::array<char, 128> asset_search_{};   // the Assets panel's search text
    std::array<char, 1024> asset_path_text_{};      // the new source path for a rename, for the selected asset
    std::array<char, 4096> asset_settings_text_{};  // the import settings text of the selected asset
    std::string asset_detail_key_;           // the GUID whose path and settings text are loaded
    std::string asset_status_;               // the last asset command's error, or empty
    // A rename or delete waits for its confirmation. The references are listed at the moment it is asked for.
    bool asset_action_rename_ = false;
    std::string asset_action_guid_;
    std::string asset_action_target_;
    std::vector<std::string> asset_action_references_;
};

}  // namespace engine::editor
