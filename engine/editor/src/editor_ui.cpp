#include "engine/editor/editor_ui.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>
#include <string>
#include <string_view>
#include <variant>

namespace engine::editor {

namespace {

constexpr ImVec4 kErrorColor(1.0f, 0.45f, 0.45f, 1.0f);
constexpr ImVec4 kWarningColor(1.0f, 0.85f, 0.35f, 1.0f);

// Copies text into a fixed buffer and always leaves it NUL-terminated, so a long path is cut rather than overrun.
template <std::size_t N>
void copy_text(std::array<char, N>& buffer, std::string_view text) {
    const std::size_t count = std::min(text.size(), N - 1);
    std::memcpy(buffer.data(), text.data(), count);
    buffer[count] = '\0';
}

// The GUID an asset payload carries, or false when the payload is not one.
bool read_guid_payload(const ImGuiPayload* payload, core::Guid& out) {
    if (payload->DataSize != static_cast<int>(core::Guid::Bytes{}.size())) {
        return false;
    }
    core::Guid::Bytes raw{};
    std::memcpy(raw.data(), payload->Data, raw.size());
    out = core::Guid(raw);
    return true;
}

const char* panel_title(Panel panel) {
    switch (panel) {
        case Panel::Project:
            return "Project";
        case Panel::Console:
            return "Console";
        case Panel::Errors:
            return "Errors";
        case Panel::Viewport:
            return "Viewport";
        case Panel::Workspace:
            return "Workspace";
    }
    return "";
}

// Filter 0 shows everything; filters 1 to 3 start at info, warnings, and errors.
bool passes_filter(core::LogLevel level, int filter) {
    static constexpr core::LogLevel kThresholds[] = {core::LogLevel::Trace, core::LogLevel::Info, core::LogLevel::Warning,
                                                     core::LogLevel::Error};
    return static_cast<int>(level) >= static_cast<int>(kThresholds[std::clamp(filter, 0, 3)]);
}

ImVec4 level_color(core::LogLevel level) {
    if (level == core::LogLevel::Error || level == core::LogLevel::Fatal) {
        return kErrorColor;
    }
    if (level == core::LogLevel::Warning) {
        return kWarningColor;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// Copies a command's error into the status line, or clears the line when the command succeeded.
template <typename T>
void report(const core::Result<T, scene::SceneError>& result, std::string& status) {
    status = result.ok() ? std::string() : result.error().message;
}

// A short label for a GUID in the hierarchy; the full text is in the inspector.
std::string short_guid(const core::Guid& guid) {
    return guid.to_string().substr(0, 8);
}

// Entity GUIDs travel in drag and drop as their 16 raw bytes.
constexpr const char* kEntityPayload = "ENTITY_GUID";
// An asset dragged from the Assets panel carries its GUID's raw bytes, as an entity does.
constexpr const char* kAssetPayload = "ASSET_GUID";
constexpr const char* kAssetsTitle = "Assets";
constexpr float kHierarchyIndent = 14.0f;
constexpr const char* kUnsavedTitle = "Unsaved changes";
constexpr ImVec4 kOkColor(0.55f, 0.9f, 0.55f, 1.0f);

void draw_entry(const ConsoleEntry& entry) {
    const std::string level(core::to_string(entry.level));
    ImGui::TextColored(level_color(entry.level), "[%s] %s: %s", level.c_str(), entry.category.c_str(), entry.message.c_str());
}

}  // namespace

EditorUi::EditorUi(EditorModel& model) : model_(model) {
    copy_text(path_text_, model_.workspace().last_project);
    copy_text(parent_text_, "");
    copy_text(name_text_, "");
}

void EditorUi::apply_saved_layout() {
    const std::string& layout = model_.workspace().layout;
    if (!layout.empty()) {
        ImGui::LoadIniSettingsFromMemory(layout.c_str(), layout.size());
    }
    default_layout_needed_ = layout.empty();
}

void EditorUi::capture_layout() {
    model_.workspace().layout = ImGui::SaveIniSettingsToMemory();
}

void EditorUi::history_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    // A text field keeps its own undo, so the scene's history waits until no text field has the keyboard.
    if (io.WantTextInput || !io.KeyCtrl || !model_.scene_editable()) {
        return;
    }
    SceneEditor& scene = model_.scene();
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        static_cast<void>(io.KeyShift ? scene.redo() : scene.undo());
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        static_cast<void>(scene.redo());
    }
}

void EditorUi::build() {
    if (exit_requested_) {
        exit_requested_ = false;
        request(PendingAction::Exit);
    }
    model_.tick_play(static_cast<double>(ImGui::GetIO().DeltaTime));
    model_.tick_assets();
    history_shortcuts();
    dock_space();
    // The dialog is opened and drawn outside the host window, so its popup ID is the same in both calls.
    unsaved_dialog();
}

void EditorUi::dock_space() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EditorHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    menu_bar();

    const ImGuiID dock_id = ImGui::GetID("EditorDockSpace");
    // The reset comes from the View menu above, so it applies on this same frame.
    if (model_.take_layout_reset() || default_layout_needed_) {
        default_layout(dock_id);
        default_layout_needed_ = false;
    }
    ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    project_panel();
    assets_panel();
    viewport_panel();
    console_panel();
    errors_panel();
    material_graph_window();
    animation_graph_window();
    terrain_window();
    world_window();
    build_game_window();
    workspace_panel();
    hierarchy_panel();
    inspector_panel();
    ImGui::End();
}

void EditorUi::default_layout(unsigned int dock_id) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dock_id);
    ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock_id, viewport->WorkSize);

    // Left column for the project and workspace; the centre for the viewport; a strip at the bottom for the console and errors.
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGui::DockBuilderSplitNode(dock_id, ImGuiDir_Left, 0.22f, &left, &right);
    ImGuiID bottom = 0;
    ImGuiID center = 0;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.30f, &bottom, &center);
    ImGuiID errors = 0;
    ImGuiID console = 0;
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.35f, &errors, &console);
    ImGuiID inspector = 0;
    ImGuiID viewport_area = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &inspector, &viewport_area);

    ImGui::DockBuilderDockWindow(panel_title(Panel::Project), left);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow(kAssetsTitle, left);
    ImGui::DockBuilderDockWindow(panel_title(Panel::Workspace), left);
    ImGui::DockBuilderDockWindow(panel_title(Panel::Viewport), viewport_area);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    ImGui::DockBuilderDockWindow(panel_title(Panel::Console), console);
    ImGui::DockBuilderDockWindow(panel_title(Panel::Errors), errors);
    ImGui::DockBuilderFinish(dock_id);
}

void EditorUi::menu_bar() {
    if (!ImGui::BeginMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene")) {
            request(PendingAction::NewScene);
        }
        if (ImGui::MenuItem("Save scene")) {
            static_cast<void>(save_scene());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save workspace")) {
            capture_layout();
            static_cast<void>(model_.save_workspace());
        }
        if (ImGui::MenuItem("Build game...", nullptr, false, model_.project().state() == ProjectState::Open)) {
            build_open_ = true;
        }
        if (ImGui::MenuItem("Close project", nullptr, false, model_.project().state() == ProjectState::Open)) {
            request(PendingAction::CloseProject);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            request(PendingAction::Exit);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        SceneEditor& scene = model_.scene();
        const std::string undo = scene.can_undo() ? "Undo " + std::string(scene.undo_label()) : std::string("Undo");
        const std::string redo = scene.can_redo() ? "Redo " + std::string(scene.redo_label()) : std::string("Redo");
        if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, scene.can_undo() && model_.scene_editable())) {
            static_cast<void>(scene.undo());
        }
        if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, scene.can_redo() && model_.scene_editable())) {
            static_cast<void>(scene.redo());
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (const Panel panel : {Panel::Project, Panel::Viewport, Panel::Console, Panel::Errors, Panel::Workspace}) {
            bool visible = model_.panel_visible(panel);
            if (ImGui::MenuItem(panel_title(panel), nullptr, &visible)) {
                model_.set_panel_visible(panel, visible);
            }
        }
        ImGui::MenuItem("Material Graph", nullptr, &material_graph_open_);
        ImGui::MenuItem("Animation Graph", nullptr, &animation_graph_open_);
        ImGui::MenuItem("Terrain", nullptr, &terrain_open_);
        ImGui::MenuItem("World", nullptr, &world_open_);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            model_.reset_layout();
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void EditorUi::project_panel() {
    if (!model_.panel_visible(Panel::Project)) {
        return;
    }
    bool open = true;
    if (ImGui::Begin(panel_title(Panel::Project), &open)) {
        const ProjectSession& session = model_.project();
        switch (session.state()) {
            case ProjectState::Closed:
                ImGui::TextUnformatted("No project is open.");
                break;
            case ProjectState::Invalid:
                ImGui::TextColored(kErrorColor, "The project cannot be opened: %s", session.error()->message.c_str());
                break;
            case ProjectState::Open:
                ImGui::Text("Project: %s", session.info()->name.c_str());
                ImGui::TextUnformatted(session.info()->root.str().c_str());
                break;
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Open a project folder");
        ImGui::InputText("Folder##open", path_text_.data(), path_text_.size());
        if (ImGui::Button("Open")) {
            static_cast<void>(model_.open_project(path_text_.data()));
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Create a project");
        ImGui::InputText("Parent folder##create", parent_text_.data(), parent_text_.size());
        ImGui::InputText("Name##create", name_text_.data(), name_text_.size());
        if (ImGui::Button("Create")) {
            const auto created = model_.create_project(parent_text_.data(), name_text_.data());
            if (created.ok()) {
                copy_text(path_text_, model_.workspace().last_project);
            }
        }
        if (model_.project().state() == ProjectState::Open) {
            ImGui::SameLine();
            if (ImGui::Button("Close project")) {
                request(PendingAction::CloseProject);
            }
        }

        if (const assets::ProjectScan* scan = model_.project().scan()) {
            ImGui::Separator();
            ImGui::Text("Registered assets: %zu", scan->registry.size());
            if (const auto& scan_error = model_.project().scan_error()) {
                ImGui::TextColored(kWarningColor, "Assets: %s", scan_error->message.c_str());
            }
            // Missing and unregistered files are listed, not fatal: the editor stays usable with them.
            if (!scan->orphan_metadata.empty()) {
                ImGui::TextColored(kWarningColor, "Metadata without a source (%zu):", scan->orphan_metadata.size());
                for (const std::string& path : scan->orphan_metadata) {
                    ImGui::BulletText("%s", path.c_str());
                }
            }
            if (!scan->unregistered.empty()) {
                ImGui::TextColored(kWarningColor, "Sources without metadata (%zu):", scan->unregistered.size());
                for (const std::string& path : scan->unregistered) {
                    ImGui::BulletText("%s", path.c_str());
                }
            }
        }
    }
    ImGui::End();
    if (!open) {
        model_.set_panel_visible(Panel::Project, false);
    }
}

void EditorUi::console_panel() {
    if (!model_.panel_visible(Panel::Console)) {
        return;
    }
    bool open = true;
    if (ImGui::Begin(panel_title(Panel::Console), &open)) {
        static constexpr const char* kFilters[] = {"All", "Info and up", "Warnings and up", "Errors only"};
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("Level##console", &level_filter_, kFilters, IM_ARRAYSIZE(kFilters));
        ImGui::SameLine();
        if (ImGui::Button("Clear##console")) {
            model_.console().clear();
        }
        if (model_.console().dropped() != 0) {
            ImGui::SameLine();
            ImGui::Text("Dropped %llu older entries", static_cast<unsigned long long>(model_.console().dropped()));
        }
        ImGui::Separator();
        // The child takes the rest of the panel; it is ended whether or not it is shown.
        ImGui::BeginChild("##console_log", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (const ConsoleEntry& entry : model_.console().snapshot()) {
            if (passes_filter(entry.level, level_filter_)) {
                draw_entry(entry);
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open) {
        model_.set_panel_visible(Panel::Console, false);
    }
}

void EditorUi::errors_panel() {
    if (!model_.panel_visible(Panel::Errors)) {
        return;
    }
    bool open = true;
    if (ImGui::Begin(panel_title(Panel::Errors), &open)) {
        // Errors are the warning, error, and fatal records in the same console.
        std::size_t count = 0;
        const auto entries = model_.console().snapshot();
        for (const ConsoleEntry& entry : entries) {
            if (passes_filter(entry.level, 2)) {
                ++count;
            }
        }
        ImGui::Text("%zu warnings and errors", count);
        ImGui::SameLine();
        if (ImGui::Button("Clear##errors")) {
            model_.console().clear();
        }
        ImGui::Separator();
        ImGui::BeginChild("##error_list", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (const ConsoleEntry& entry : entries) {
            if (passes_filter(entry.level, 2)) {
                draw_entry(entry);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open) {
        model_.set_panel_visible(Panel::Errors, false);
    }
}

// The grid, the entity markers, and the gizmo, drawn over the viewport's surface. The positions come from the model's camera.
static void draw_viewport_scene(EditorModel& model, ImDrawList* draw, ImVec2 origin, ImVec2 size) {
    const EditorViewport& viewport = model.viewport();
    const ViewportSurface surface{size.x, size.y, model.dpi_scale()};
    const glm::dmat4 view_projection = viewport.camera().view_projection(surface);
    const float dpi = static_cast<float>(model.dpi_scale());
    const auto to_screen = [&](const glm::dvec3& world) -> std::optional<ImVec2> {
        const std::optional<glm::dvec2> px = project_point(view_projection, surface, world);
        if (!px) {
            return std::nullopt;
        }
        return ImVec2(origin.x + static_cast<float>(px->x), origin.y + static_cast<float>(px->y));
    };
    const auto segment = [&](const glm::dvec3& a, const glm::dvec3& b, ImU32 colour, float thickness) {
        const std::optional<ImVec2> start = to_screen(a);
        const std::optional<ImVec2> end = to_screen(b);
        if (start && end) {
            draw->AddLine(*start, *end, colour, thickness);
        }
    };

    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 30, 34, 255));

    // A 20 metre grid on the ground plane, centred on the camera target so the view always shows ground near the orbit point.
    const glm::dvec3 target = viewport.camera().target;
    const double cx = std::round(target.x);
    const double cz = std::round(target.z);
    for (int i = -10; i <= 10; ++i) {
        const ImU32 colour = i == 0 ? IM_COL32(120, 120, 130, 255) : IM_COL32(64, 66, 74, 255);
        segment(glm::dvec3(cx + i, 0.0, cz - 10.0), glm::dvec3(cx + i, 0.0, cz + 10.0), colour, 1.0f);
        segment(glm::dvec3(cx - 10.0, 0.0, cz + i), glm::dvec3(cx + 10.0, 0.0, cz + i), colour, 1.0f);
    }

    // One marker per entity at its world position; the selected one is outlined.
    const std::optional<core::Guid> selected = model.scene().selected();
    for (const HierarchyRow& row : model.scene().hierarchy("")) {
        const std::optional<glm::dmat4> world = model.scene().world_matrix(row.guid);
        if (!world) {
            continue;
        }
        const std::optional<ImVec2> at = to_screen(glm::dvec3((*world)[3]));
        if (!at) {
            continue;
        }
        const bool is_selected = selected && *selected == row.guid;
        draw->AddRectFilled(ImVec2(at->x - 3.0f * dpi, at->y - 3.0f * dpi), ImVec2(at->x + 3.0f * dpi, at->y + 3.0f * dpi),
                            IM_COL32(200, 200, 210, 255));
        if (is_selected) {
            draw->AddRect(ImVec2(at->x - 7.0f * dpi, at->y - 7.0f * dpi), ImVec2(at->x + 7.0f * dpi, at->y + 7.0f * dpi),
                          IM_COL32(255, 220, 60, 255), 0.0f, 0, 2.0f * dpi);
        }
    }

    const std::optional<GizmoLayout>& gizmo = viewport.gizmo();
    if (!gizmo) {
        return;
    }
    constexpr std::array<ImU32, 3> kAxisColours{IM_COL32(230, 75, 75, 255), IM_COL32(90, 205, 90, 255), IM_COL32(85, 135, 245, 255)};
    const ImU32 kActive = IM_COL32(255, 230, 90, 255);
    // The hovered handle, or the one being dragged, is drawn in the highlight colour.
    const auto highlighted = [&](GizmoHandle handle) { return gizmo->hovered && *gizmo->hovered == handle; };
    const float thickness = 3.0f * dpi;
    if (viewport.mode() == GizmoMode::Rotate) {
        constexpr std::size_t kRingPoints = 64;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::vector<glm::dvec3> ring = viewport.ring_points(axis, kRingPoints);
            const GizmoHandle handle = axis == 0 ? GizmoHandle::X : axis == 1 ? GizmoHandle::Y : GizmoHandle::Z;
            const ImU32 colour = highlighted(handle) ? kActive : kAxisColours[axis];
            for (std::size_t k = 0; k < ring.size(); ++k) {
                segment(ring[k], ring[(k + 1) % ring.size()], colour, thickness);
            }
        }
    } else {
        const std::optional<ImVec2> pivot = to_screen(gizmo->origin);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const GizmoHandle handle = axis == 0 ? GizmoHandle::X : axis == 1 ? GizmoHandle::Y : GizmoHandle::Z;
            const ImU32 colour = highlighted(handle) ? kActive : kAxisColours[axis];
            segment(gizmo->origin, gizmo->origin + gizmo->axes[axis] * gizmo->length, colour, thickness);
            if (const std::optional<ImVec2> tip = to_screen(gizmo->origin + gizmo->axes[axis] * gizmo->length)) {
                draw->AddCircleFilled(*tip, 5.0f * dpi, colour);
            }
        }
        if (pivot) {
            draw->AddCircleFilled(*pivot, 5.0f * dpi, highlighted(GizmoHandle::Centre) ? kActive : IM_COL32(245, 245, 245, 255));
        }
    }
}

void EditorUi::material_graph_window() {
    if (material_graph_open_) {
        material_graph_.draw(material_graph_open_);
    }
}

void EditorUi::animation_graph_window() {
    if (animation_graph_open_) {
        PlaySession* play = model_.play_session();
        animation_graph_.draw(animation_graph_open_, play != nullptr ? &play->animators() : nullptr);
    }
}

void EditorUi::terrain_window() {
    if (terrain_open_) {
        terrain_.draw(terrain_open_);
    }
}

void EditorUi::world_window() {
    if (world_open_) {
        world_.draw(world_open_);
    }
}

// Build Game (US-073). The build runs on a worker; the window polls it each frame and shows the stage, the cook progress, and the
// outcome. Cancel stops the build before its next step, and the staged package is removed.
void EditorUi::build_game_window() {
    if (!build_open_) {
        return;
    }
    if (auto result = game_build_.poll()) {
        if (result->ok()) {
            const auto& report = result->value();
            build_status_ = "Built " + report.package_dir + " (" + std::to_string(report.files) + " files, " +
                            std::to_string(report.pack_entries) + " assets" + (report.verified ? ", launch verified)" : ")");
            model_.console().add(core::LogLevel::Info, "build", build_status_);
        } else {
            build_status_ = "Build failed: " + result->error();
            model_.console().add(core::LogLevel::Error, "build", build_status_);
        }
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Build Game", &build_open_)) {
        const bool idle = !game_build_.running();
        ImGui::BeginDisabled(!idle);
        ImGui::InputText("Output folder##build", build_output_.data(), build_output_.size());
        ImGui::InputText("Package name##build", build_name_.data(), build_name_.size());
        ImGui::InputText("Start scene GUID##build", build_scene_.data(), build_scene_.size());
        const char* backends[] = {"dx12", "vulkan"};
        ImGui::Combo("Backend##build", &build_backend_, backends, 2);
        ImGui::EndDisabled();
        if (idle && ImGui::Button("Build")) {
            const auto project = model_.project().info();
            const auto guid = core::Guid::parse(build_scene_.data());
            if (!project.has_value()) {
                build_status_ = "Open a project first";
            } else if (!guid.ok()) {
                build_status_ = "The start scene must be a GUID";
            } else {
                GameBuildOptions options;
                options.project = project->root.str();
                options.output_dir = build_output_.data();
                options.game_name = build_name_.data();
                options.start_scene = guid.value();
                options.backend = build_backend_ == 0 ? "dx12" : "vulkan";
                options.profile = "native";
                options.runtime_exe = ENGINE_GAME_RUNTIME_EXE;
                options.shader_dir = ENGINE_BUILD_SHADER_DIR;
                options.notices_file = ENGINE_NOTICES_FILE;
                auto started = game_build_.start(options);
                build_status_ = started.ok() ? "Building..." : started.error();
            }
        }
        if (!idle) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                game_build_.cancel();
            }
            const std::size_t total = game_build_.total();
            const float fraction = total == 0 ? 0.0f : static_cast<float>(game_build_.done()) / static_cast<float>(total);
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
            ImGui::Text("%s", std::string(to_string(game_build_.stage())).c_str());
        }
        if (!build_status_.empty()) {
            ImGui::TextWrapped("%s", build_status_.c_str());
        }
    }
    ImGui::End();
}

void EditorUi::viewport_panel() {
    if (!model_.panel_visible(Panel::Viewport)) {
        return;
    }
    bool open = true;
    if (ImGui::Begin(panel_title(Panel::Viewport), &open)) {
        // Play controls (US-065). Play starts a runtime world from the scene; Stop discards it and the scene is shown as it was.
        const PlayState play_state = model_.play_state();
        const bool in_edit = play_state == PlayState::Edit;
        const bool paused = play_state == PlayState::Paused;
        ImGui::BeginDisabled(!in_edit);
        if (ImGui::Button("Play")) {
            static_cast<void>(model_.play());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(in_edit);
        if (ImGui::Button(paused ? "Resume" : "Pause")) {
            model_.toggle_pause();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!paused);
        if (ImGui::Button("Step")) {
            model_.step();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            model_.stop();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(in_edit ? "Edit" : paused ? "Paused" : "Playing");

        EditorViewport& viewport = model_.viewport();
        SnapSettings& snap = viewport.snap();
        int tool = static_cast<int>(viewport.mode());
        if (ImGui::RadioButton("Move", &tool, static_cast<int>(GizmoMode::Translate))) {
            viewport.set_mode(GizmoMode::Translate);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", &tool, static_cast<int>(GizmoMode::Rotate))) {
            viewport.set_mode(GizmoMode::Rotate);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale", &tool, static_cast<int>(GizmoMode::Scale))) {
            viewport.set_mode(GizmoMode::Scale);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("|");
        ImGui::SameLine();
        if (ImGui::RadioButton("Local", viewport.space() == GizmoSpace::Local)) {
            viewport.set_space(GizmoSpace::Local);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("World", viewport.space() == GizmoSpace::World)) {
            viewport.set_space(GizmoSpace::World);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &snap.enabled);
        if (snap.enabled) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f * static_cast<float>(model_.dpi_scale()));
            ImGui::InputDouble("m", &snap.translate_step, 0.0, 0.0, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f * static_cast<float>(model_.dpi_scale()));
            ImGui::InputDouble("deg", &snap.rotate_step_degrees, 0.0, 0.0, "%.1f");
        }
        ImGui::TextDisabled("LMB select or drag handle | RMB orbit, RMB + WASD/QE fly | MMB pan | wheel zoom | F frame");

        // The viewport surface is the rest of the panel, in framebuffer pixels: ImGui's display is in them (imgui_input.cpp).
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 size(std::max(available.x, 1.0f), std::max(available.y, 1.0f));
        ImGui::InvisibleButton("##scene_viewport", size);
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiIO& io = ImGui::GetIO();

        // An asset dropped on the viewport is placed as a new entity (US-066). The model logs why a drop is refused.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                core::Guid dropped;
                if (read_guid_payload(payload, dropped)) {
                    static_cast<void>(model_.place_asset(dropped.to_string()));
                }
            }
            ImGui::EndDragDropTarget();
        }

        // A right or middle press that starts over the viewport keeps its drag until the button is released.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            orbit_captured_ = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            orbit_captured_ = false;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            pan_captured_ = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            pan_captured_ = false;
        }

        ViewportInput input;
        input.surface = ViewportSurface{static_cast<double>(size.x), static_cast<double>(size.y), model_.dpi_scale()};
        input.hovered = hovered;
        if (io.MousePos.x > -1.0e5f) {
            input.pointer = glm::dvec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        }
        input.pointer_delta = glm::dvec2(io.MouseDelta.x, io.MouseDelta.y);
        input.primary_pressed = ImGui::IsItemActivated();
        input.primary_down = ImGui::IsItemActive();
        input.orbit_down = orbit_captured_;
        input.pan_down = pan_captured_;
        input.wheel = hovered ? static_cast<double>(io.MouseWheel) : 0.0;
        if (orbit_captured_) {
            input.fly = glm::dvec3((ImGui::IsKeyDown(ImGuiKey_D) ? 1.0 : 0.0) - (ImGui::IsKeyDown(ImGuiKey_A) ? 1.0 : 0.0),
                                   (ImGui::IsKeyDown(ImGuiKey_E) ? 1.0 : 0.0) - (ImGui::IsKeyDown(ImGuiKey_Q) ? 1.0 : 0.0),
                                   (ImGui::IsKeyDown(ImGuiKey_W) ? 1.0 : 0.0) - (ImGui::IsKeyDown(ImGuiKey_S) ? 1.0 : 0.0));
        }
        input.delta_seconds = io.DeltaTime;
        input.frame_selection = hovered && ImGui::IsKeyPressed(ImGuiKey_F, false);
        model_.update_viewport(input);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
        draw_viewport_scene(model_, draw, origin, size);
        const std::string_view mode_name = viewport.mode() == GizmoMode::Translate ? "Move" : viewport.mode() == GizmoMode::Rotate ? "Rotate" : "Scale";
        const std::string_view space_name = viewport.space() == GizmoSpace::Local ? "Local" : "World";
        const std::string label = std::string(mode_name) + " | " + std::string(space_name) + (snap.enabled ? " | snap on" : "");
        draw->AddText(ImVec2(origin.x + 8.0f, origin.y + 6.0f), IM_COL32(220, 220, 225, 255), label.c_str());
        draw->PopClipRect();
    }
    ImGui::End();
    if (!open) {
        model_.set_panel_visible(Panel::Viewport, false);
    }
}

void EditorUi::workspace_panel() {
    if (!model_.panel_visible(Panel::Workspace)) {
        return;
    }
    bool open = true;
    if (ImGui::Begin(panel_title(Panel::Workspace), &open)) {
        ImGui::TextWrapped("Workspace file: %s", model_.workspace_file().str().c_str());
        if (ImGui::Button("Save workspace")) {
            capture_layout();
            static_cast<void>(model_.save_workspace());
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset layout")) {
            model_.reset_layout();
        }
        ImGui::Separator();
        // Cyrillic is in the editor's font (Roboto, with the Cyrillic glyph range); this line shows it renders.
        ImGui::TextUnformatted("Кириллица: Проект, Консоль, Ошибки. Ёж, щука, объект.");
    }
    ImGui::End();
    if (!open) {
        model_.set_panel_visible(Panel::Workspace, false);
    }
}

namespace {

// The scene file named in the text box, or nothing with the reason in status.
std::optional<core::Path> scene_file(std::string_view text, std::string& status) {
    auto path = core::Path::from_utf8(text);
    if (!path.ok()) {
        status = "the scene file path is not valid UTF-8";
        return std::nullopt;
    }
    return path.value();
}

// The range of a numeric field, for its tooltip. Empty when the field has no bounds.
std::string range_text(const scene::Range& range) {
    if (!range.min && !range.max) {
        return {};
    }
    const std::string low = range.min ? std::to_string(*range.min) : std::string("any");
    const std::string high = range.max ? std::to_string(*range.max) : std::string("any");
    return "range " + low + " to " + high;
}

}  // namespace

void EditorUi::request(PendingAction action) {
    if (action == PendingAction::None || !model_.scene().dirty()) {
        perform(action);
        return;
    }
    pending_ = action;
    dialog_status_.clear();
    open_dialog_ = true;
}

void EditorUi::perform(PendingAction action) {
    model_.stop();
    switch (action) {
        case PendingAction::None:
            return;
        case PendingAction::NewScene:
            model_.scene().new_scene();
            scene_status_.clear();
            return;
        case PendingAction::OpenScene:
            open_scene();
            return;
        case PendingAction::CloseProject:
            model_.close_project();
            return;
        case PendingAction::Exit:
            model_.request_quit();
            return;
    }
}

bool EditorUi::save_scene() {
    if (scene_text_[0] == '\0') {
        report(model_.scene().save(), scene_status_);
    } else if (auto path = scene_file(scene_text_.data(), scene_status_)) {
        report(model_.scene().save_as(*path), scene_status_);
    }
    return scene_status_.empty();
}

void EditorUi::open_scene() {
    if (auto path = scene_file(scene_text_.data(), scene_status_)) {
        report(model_.scene().open(*path), scene_status_);
    }
}

void EditorUi::unsaved_dialog() {
    if (open_dialog_) {
        ImGui::OpenPopup(kUnsavedTitle);
        open_dialog_ = false;
    }
    if (!ImGui::BeginPopupModal(kUnsavedTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("The scene has unsaved changes.");
    ImGui::TextUnformatted("Save them, discard them, or cancel.");
    if (!dialog_status_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", dialog_status_.c_str());
    }
    if (ImGui::Button("Save")) {
        const PendingAction action = pending_;
        if (save_scene()) {
            pending_ = PendingAction::None;
            ImGui::CloseCurrentPopup();
            perform(action);
        } else {
            dialog_status_ = scene_status_;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        const PendingAction action = pending_;
        pending_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
        perform(action);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        pending_ = PendingAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorUi::hierarchy_panel() {
    SceneEditor& scene = model_.scene();
    if (ImGui::Begin("Hierarchy")) {
        // While a play world runs, the scene is read-only: the hierarchy shows it without changing it (docs/editor.md).
        ImGui::BeginDisabled(!model_.scene_editable());
        if (scene.path()) {
            ImGui::Text("Scene: %s", scene.path()->str().c_str());
        } else {
            ImGui::TextUnformatted("Scene: not saved yet");
        }
        if (scene.dirty()) {
            ImGui::TextColored(kWarningColor, "Unsaved changes");
        }
        ImGui::InputText("File##scene", scene_text_.data(), scene_text_.size());
        if (ImGui::Button("New")) {
            request(PendingAction::NewScene);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open")) {
            request(PendingAction::OpenScene);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            static_cast<void>(save_scene());
        }

        ImGui::Separator();
        ImGui::InputText("Search##hierarchy", search_text_.data(), search_text_.size());
        const std::optional<core::Guid> selected = scene.selected();
        if (ImGui::Button("New entity")) {
            report(scene.create_entity(), scene_status_);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected);
        if (ImGui::Button("Duplicate") && selected) {
            report(scene.duplicate(*selected), scene_status_);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete") && selected) {
            report(scene.remove(*selected), scene_status_);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected || scene.is_instance_interior(selected.value_or(core::Guid{})) ||
                             scene.scene().parent(selected.value_or(core::Guid{})).is_nil());
        if (ImGui::Button("Move to root") && selected) {
            report(scene.reparent(*selected, core::Guid{}), scene_status_);
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::BeginChild("##entities", ImVec2(0.0f, 0.0f), false);
        for (const HierarchyRow& row : scene.hierarchy(search_text_.data())) {
            ImGui::PushID(row.guid.to_string().c_str());
            const float indent = kHierarchyIndent * static_cast<float>(row.depth);
            ImGui::Indent(indent);
            const std::string label = short_guid(row.guid) + (row.prefab_instance ? " [prefab]" : "");
            if (ImGui::Selectable(label.c_str(), selected && *selected == row.guid)) {
                scene.select(row.guid);
            }
            // Dropping an entity on a row makes it a child of that row. Dragging it onto a row it would cycle through is refused.
            if (ImGui::BeginDragDropSource()) {
                const core::Guid::Bytes& bytes = row.guid.bytes();
                ImGui::SetDragDropPayload(kEntityPayload, bytes.data(), bytes.size());
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                    if (payload->DataSize == static_cast<int>(core::Guid::Bytes{}.size())) {
                        core::Guid::Bytes raw{};
                        std::memcpy(raw.data(), payload->Data, raw.size());
                        report(scene.reparent(core::Guid(raw), row.guid), scene_status_);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::Unindent(indent);
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (!scene_status_.empty()) {
            ImGui::TextColored(kErrorColor, "%s", scene_status_.c_str());
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorUi::inspector_panel() {
    SceneEditor& scene = model_.scene();
    if (ImGui::Begin("Inspector")) {
        ImGui::BeginDisabled(!model_.scene_editable());
        const std::optional<core::Guid> selected = scene.selected();
        if (!selected) {
            ImGui::TextUnformatted("Select an entity in the hierarchy to edit it.");
        } else {
            const core::Guid entity = *selected;
            const bool interior = scene.is_instance_interior(entity);
            const bool prefab_instance = scene.scene().prefab_of(entity).has_value();
            ImGui::Text("Entity %s", entity.to_string().c_str());
            if (interior) {
                ImGui::TextWrapped("Inside a prefab instance: its transform and components come from the prefab. Field values can be overridden.");
            } else if (prefab_instance) {
                ImGui::TextWrapped("Prefab instance: its components come from the prefab and it cannot gain or lose any.");
            }
            ImGui::Separator();
            transform_section(entity, interior);
            ImGui::Separator();
            component_section(entity, prefab_instance);
            if (!scene_status_.empty()) {
                ImGui::TextColored(kErrorColor, "%s", scene_status_.c_str());
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
    // A value typed or dragged in the inspector is one undo step: the item's edits merge until it is released.
    const bool editing = ImGui::IsAnyItemActive();
    if (editing != inspector_gesture_) {
        inspector_gesture_ = editing;
        if (editing) {
            model_.scene().begin_gesture();
        } else {
            model_.scene().end_gesture();
        }
    }
}

void EditorUi::transform_section(const core::Guid& entity, bool read_only) {
    const scene::Transform* current = model_.scene().scene().transform(entity);
    if (current == nullptr) {
        return;
    }
    // Rotation is edited as Euler angles in degrees: any value the user types makes a unit quaternion, so the transform stays valid.
    const glm::dvec3 euler = glm::degrees(glm::eulerAngles(current->rotation));
    double position[3] = {current->position.x, current->position.y, current->position.z};
    double rotation[3] = {euler.x, euler.y, euler.z};
    double scale[3] = {current->scale.x, current->scale.y, current->scale.z};
    ImGui::TextUnformatted("Transform");
    ImGui::BeginDisabled(read_only);
    bool changed = ImGui::InputScalarN("Position##transform", ImGuiDataType_Double, position, 3, nullptr, nullptr, "%.3f");
    changed = ImGui::InputScalarN("Rotation (deg)##transform", ImGuiDataType_Double, rotation, 3, nullptr, nullptr, "%.2f") || changed;
    changed = ImGui::InputScalarN("Scale##transform", ImGuiDataType_Double, scale, 3, nullptr, nullptr, "%.3f") || changed;
    ImGui::EndDisabled();
    if (changed) {
        scene::Transform edit;
        edit.position = glm::dvec3(position[0], position[1], position[2]);
        edit.rotation = glm::dquat(glm::radians(glm::dvec3(rotation[0], rotation[1], rotation[2])));
        edit.scale = glm::dvec3(scale[0], scale[1], scale[2]);
        report(model_.scene().set_transform(entity, edit), scene_status_);
    }
}

void EditorUi::component_section(const core::Guid& entity, bool read_only) {
    SceneEditor& scene = model_.scene();
    ImGui::TextUnformatted("Components");
    std::optional<std::string> to_remove;
    for (const std::string& type_id : scene.scene().component_types(entity)) {
        ImGui::PushID(type_id.c_str());
        const scene::ComponentDescriptor* descriptor = scene.catalog().find(type_id);
        if (ImGui::CollapsingHeader(type_id.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (descriptor == nullptr) {
                ImGui::TextColored(kWarningColor, "No descriptor is registered for this component, so its fields cannot be shown.");
            } else {
                for (const scene::FieldDescriptor& field : descriptor->fields) {
                    field_editor(entity, *descriptor, field);
                }
            }
            ImGui::BeginDisabled(read_only);
            if (ImGui::Button("Remove component")) {
                to_remove = type_id;
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    if (to_remove) {
        report(scene.remove_component(entity, *to_remove), scene_status_);
    }
    add_component_row(entity, read_only);
}

void EditorUi::add_component_row(const core::Guid& entity, bool read_only) {
    SceneEditor& scene = model_.scene();
    const std::vector<std::string> present = scene.scene().component_types(entity);
    std::vector<const scene::ComponentDescriptor*> available;
    for (const scene::ComponentDescriptor& descriptor : scene.catalog().components()) {
        if (std::find(present.begin(), present.end(), descriptor.type_id) == present.end()) {
            available.push_back(&descriptor);
        }
    }
    if (available.empty()) {
        ImGui::TextDisabled(scene.catalog().size() == 0 ? "No component types are registered." : "Every registered component is on this entity.");
        return;
    }
    add_component_index_ = std::clamp(add_component_index_, 0, static_cast<int>(available.size()) - 1);
    ImGui::BeginDisabled(read_only);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##add_component", available[static_cast<std::size_t>(add_component_index_)]->type_id.c_str())) {
        for (std::size_t i = 0; i < available.size(); ++i) {
            if (ImGui::Selectable(available[i]->type_id.c_str(), static_cast<int>(i) == add_component_index_)) {
                add_component_index_ = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add component")) {
        report(scene.add_component(entity, available[static_cast<std::size_t>(add_component_index_)]->type_id), scene_status_);
    }
    ImGui::EndDisabled();
}

void EditorUi::field_editor(const core::Guid& entity, const scene::ComponentDescriptor& descriptor, const scene::FieldDescriptor& field) {
    const std::optional<scene::FieldValue> value = model_.scene().scene().field(entity, descriptor.type_id, field.name);
    if (!value) {
        return;
    }
    const char* label = field.name.c_str();
    // Each edit goes through the scene, which checks the value against the descriptor. A rejected value is shown in the status line.
    auto apply = [&](const scene::FieldValue& edit) {
        report(model_.scene().set_field(entity, descriptor.type_id, field.name, edit), scene_status_);
    };
    ImGui::PushID(label);
    if (const auto* bool_value = std::get_if<bool>(&*value)) {
        bool edit = *bool_value;
        if (ImGui::Checkbox(label, &edit)) {
            apply(scene::FieldValue(std::in_place_type<bool>, edit));
        }
    } else if (const auto* int_value = std::get_if<std::int32_t>(&*value)) {
        int edit = *int_value;
        if (ImGui::InputInt(label, &edit)) {
            apply(scene::FieldValue(std::in_place_type<std::int32_t>, static_cast<std::int32_t>(edit)));
        }
    } else if (const auto* uint_value = std::get_if<std::uint32_t>(&*value)) {
        std::uint32_t edit = *uint_value;
        if (ImGui::InputScalar(label, ImGuiDataType_U32, &edit)) {
            apply(scene::FieldValue(std::in_place_type<std::uint32_t>, edit));
        }
    } else if (const auto* float_value = std::get_if<float>(&*value)) {
        float edit = *float_value;
        if (ImGui::InputFloat(label, &edit, 0.0f, 0.0f, "%.4f")) {
            apply(scene::FieldValue(std::in_place_type<float>, edit));
        }
    } else if (const auto* vec2_value = std::get_if<std::array<float, 2>>(&*value)) {
        std::array<float, 2> edit = *vec2_value;
        if (ImGui::InputFloat2(label, edit.data())) {
            apply(scene::FieldValue(std::in_place_type<std::array<float, 2>>, edit));
        }
    } else if (const auto* vec3_value = std::get_if<std::array<float, 3>>(&*value)) {
        std::array<float, 3> edit = *vec3_value;
        if (ImGui::InputFloat3(label, edit.data())) {
            apply(scene::FieldValue(std::in_place_type<std::array<float, 3>>, edit));
        }
    } else if (const auto* vec4_value = std::get_if<std::array<float, 4>>(&*value)) {
        std::array<float, 4> edit = *vec4_value;
        if (ImGui::InputFloat4(label, edit.data())) {
            apply(scene::FieldValue(std::in_place_type<std::array<float, 4>>, edit));
        }
    } else if (const auto* guid_value = std::get_if<core::Guid>(&*value)) {
        guid_field(entity, descriptor, field, *guid_value);
    }
    const std::string range = range_text(field.range);
    if (!range.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", range.c_str());
    }
    ImGui::PopID();
}

void EditorUi::guid_field(const core::Guid& entity, const scene::ComponentDescriptor& descriptor, const scene::FieldDescriptor& field,
                          const core::Guid& value) {
    // The text is kept while the field is edited; it is reloaded when the field changes.
    const std::string key = entity.to_string() + "/" + descriptor.type_id + "/" + field.name;
    if (guid_key_ != key) {
        guid_key_ = key;
        copy_text(guid_text_, value.is_nil() ? std::string() : value.to_string());
    }
    ImGui::InputText(field.name.c_str(), guid_text_.data(), guid_text_.size());
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string text(guid_text_.data());
        if (text.empty()) {
            set_guid_field(entity, descriptor, field, core::Guid{});
        } else if (auto parsed = core::Guid::parse(text); parsed.ok()) {
            set_guid_field(entity, descriptor, field, parsed.value());
        } else {
            scene_status_ = "\"" + text + "\" is not a GUID; use 8-4-4-4-12 hexadecimal digits";
        }
    }
    // An asset dragged from the Assets panel sets the reference. Only a registered asset is accepted.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            core::Guid dropped;
            if (read_guid_payload(payload, dropped)) {
                const CatalogEntry* asset = model_.assets().find(dropped.to_string());
                if (asset != nullptr && asset->registered()) {
                    set_guid_field(entity, descriptor, field, dropped);
                } else {
                    scene_status_ = "only a registered asset can be referenced";
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (value.is_nil()) {
        ImGui::TextDisabled("No asset");
    } else if (model_.scene().is_known_asset(value)) {
        ImGui::TextColored(kOkColor, "Asset %s", value.to_string().c_str());
    } else {
        ImGui::TextColored(kErrorColor, "Missing asset %s", value.to_string().c_str());
    }
}

void EditorUi::set_guid_field(const core::Guid& entity, const scene::ComponentDescriptor& descriptor, const scene::FieldDescriptor& field,
                              const core::Guid& value) {
    report(model_.scene().set_field(entity, descriptor.type_id, field.name, scene::FieldValue(std::in_place_type<core::Guid>, value)),
           scene_status_);
}

void EditorUi::assets_panel() {
    if (ImGui::Begin(kAssetsTitle)) {
        AssetBrowser& assets = model_.assets();
        if (!assets.is_open()) {
            ImGui::TextUnformatted("Open a project to browse its assets.");
        } else {
            assets_body();
            // The confirmation is opened and drawn inside this window, so both use the same popup ID.
            asset_action_dialog();
        }
    }
    ImGui::End();
}

void EditorUi::assets_body() {
    AssetBrowser& assets = model_.assets();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##asset_search", "Search by path, importer, or GUID", asset_search_.data(), asset_search_.size());

    // Import runs on a worker. Its progress is the count of finished assets; cancel stops before the next asset starts.
    const bool running = assets.importing();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Import")) {
        auto started = model_.import_assets();
        asset_status_ = started.ok() ? std::string() : started.error();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel import")) {
        assets.cancel_import();
    }
    ImGui::EndDisabled();
    if (running) {
        const std::size_t total = assets.import_total();
        const float fraction = total == 0 ? 1.0f : static_cast<float>(assets.import_done()) / static_cast<float>(total);
        const std::string overlay = std::to_string(assets.import_done()) + " / " + std::to_string(total);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay.c_str());
    } else if (assets.import_summary()) {
        ImGui::TextWrapped("%s", assets.import_summary()->c_str());
    }
    if (!asset_status_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", asset_status_.c_str());
    }

    const std::string query(asset_search_.data());
    const float list_width = std::max(ImGui::GetContentRegionAvail().x * 0.45f, 120.0f);
    ImGui::BeginChild("##asset_catalog", ImVec2(list_width, 0.0f), ImGuiChildFlags_Borders);
    const CatalogEntry* selected = assets.selected();
    for (const CatalogEntry* entry : assets.search(query)) {
        const std::string label = entry->source + (entry->registered() ? "  [" + entry->asset->importer + "]" : "  [no metadata]");
        const bool is_selected = selected != nullptr && selected->source == entry->source;
        if (ImGui::Selectable(label.c_str(), is_selected) && entry->registered()) {
            assets.select(entry->asset->guid.to_string());
        }
        // A registered asset drags its GUID to an inspector field or the viewport.
        if (entry->registered() && ImGui::BeginDragDropSource()) {
            const core::Guid::Bytes& bytes = entry->asset->guid.bytes();
            ImGui::SetDragDropPayload(kAssetPayload, bytes.data(), bytes.size());
            ImGui::TextUnformatted(entry->source.c_str());
            ImGui::EndDragDropSource();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Read after the list, so a click in the list shows in the detail on this frame.
    const CatalogEntry* chosen = assets.selected();
    ImGui::BeginChild("##asset_detail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (chosen != nullptr) {
        assets_detail(*chosen);
    } else {
        ImGui::TextDisabled("Select a registered asset to see its details.");
    }
    ImGui::EndChild();

    // The diagnostics of the last import, with their codes. Each one is also in the console.
    if (!assets.import_diagnostics().empty()) {
        ImGui::Separator();
        ImGui::Text("Import diagnostics (%zu)", assets.import_diagnostics().size());
        for (const converter::Diagnostic& diagnostic : assets.import_diagnostics()) {
            const ImVec4 color = diagnostic.severity == converter::Severity::Error ? kErrorColor : kWarningColor;
            std::string line = std::string(converter::to_string(diagnostic.code)) + ": ";
            if (!diagnostic.source.empty()) {
                line += diagnostic.source + ": ";
            }
            ImGui::TextColored(color, "%s%s", line.c_str(), diagnostic.message.c_str());
        }
    }
}

void EditorUi::assets_detail(const CatalogEntry& entry) {
    AssetBrowser& assets = model_.assets();
    const assets::AssetRecord& asset = *entry.asset;
    const std::string guid = asset.guid.to_string();

    // The path and settings text belong to one asset. They are reloaded when the selection changes, not on every rescan.
    if (asset_detail_key_ != guid) {
        asset_detail_key_ = guid;
        copy_text(asset_path_text_, entry.source);
        copy_text(asset_settings_text_, asset.importer_settings);
        asset_status_.clear();
    }

    ImGui::TextUnformatted(entry.source.c_str());
    ImGui::TextDisabled("GUID %s", guid.c_str());
    ImGui::Text("Importer: %s", asset.importer.c_str());
    ImGui::TextDisabled("Source hash %s", asset.content_hash.to_hex().c_str());
    if (entry.last) {
        ImGui::Text("Last import: %s", std::string(converter::to_string(entry.last->status)).c_str());
    } else {
        ImGui::TextDisabled("Not imported in this session");
    }
    for (const core::Guid& dependency : asset.dependencies) {
        const CatalogEntry* found = assets.find(dependency.to_string());
        if (found == nullptr || !found->registered()) {
            ImGui::TextColored(kErrorColor, "Missing dependency %s", dependency.to_string().c_str());
        } else {
            ImGui::BulletText("Depends on %s", found->source.c_str());
        }
    }

    // The preview: a texture's thumbnail, drawn as coloured cells. Other importers say why there is none.
    const Preview& preview = assets.preview(entry);
    if (preview.image) {
        const Thumbnail& image = *preview.image;
        constexpr float kCell = 2.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::uint8_t* texel = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
                const ImVec2 min(origin.x + static_cast<float>(x) * kCell, origin.y + static_cast<float>(y) * kCell);
                draw->AddRectFilled(min, ImVec2(min.x + kCell, min.y + kCell), IM_COL32(texel[0], texel[1], texel[2], 255));
            }
        }
        ImGui::Dummy(ImVec2(static_cast<float>(image.width) * kCell, static_cast<float>(image.height) * kCell));
    } else {
        ImGui::TextDisabled("%s", preview.error.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Import settings (JSON object)");
    ImGui::InputTextMultiline("##asset_settings", asset_settings_text_.data(), asset_settings_text_.size(), ImVec2(-1.0f, 110.0f));
    if (ImGui::Button("Apply settings")) {
        auto saved = model_.set_asset_settings(guid, asset_settings_text_.data());
        asset_status_ = saved.ok() ? std::string() : saved.error();
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##asset_path", asset_path_text_.data(), asset_path_text_.size());
    if (ImGui::Button("Rename")) {
        open_asset_action(true, guid, asset_path_text_.data());
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        open_asset_action(false, guid, entry.source);
    }
    ImGui::TextDisabled("Rename and delete list the references they affect before they run.");
    if (!asset_status_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", asset_status_.c_str());
    }
}

void EditorUi::open_asset_action(bool rename, const std::string& guid, const std::string& target) {
    if (rename && target.empty()) {
        asset_status_ = "enter a new path first";
        return;
    }
    auto references = model_.assets().references_to(guid);
    if (!references.ok()) {
        asset_status_ = references.error();
        return;
    }
    asset_action_rename_ = rename;
    asset_action_guid_ = guid;
    asset_action_target_ = target;
    asset_action_references_ = std::move(references).value();
    ImGui::OpenPopup("Affected references");
}

void EditorUi::asset_action_dialog() {
    if (!ImGui::BeginPopupModal("Affected references", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (asset_action_rename_) {
        ImGui::Text("Rename to %s", asset_action_target_.c_str());
        ImGui::TextDisabled("The GUID stays the same, so these references remain valid.");
    } else {
        ImGui::Text("Delete %s", asset_action_target_.c_str());
        ImGui::TextDisabled("These references will name a GUID that no longer exists.");
    }
    if (asset_action_references_.empty()) {
        ImGui::TextUnformatted("No other asset or text source names this GUID.");
    } else {
        ImGui::Text("Affected references (%zu):", asset_action_references_.size());
        for (const std::string& line : asset_action_references_) {
            ImGui::BulletText("%s", line.c_str());
        }
    }
    if (ImGui::Button("Confirm")) {
        auto done = asset_action_rename_ ? model_.rename_asset(asset_action_guid_, asset_action_target_)
                                         : model_.delete_asset(asset_action_guid_);
        asset_status_ = done.ok() ? std::string() : done.error();
        if (done.ok()) {
            // The path text belongs to the old path, so the detail reloads from the catalog on the next frame.
            asset_detail_key_.clear();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace engine::editor
