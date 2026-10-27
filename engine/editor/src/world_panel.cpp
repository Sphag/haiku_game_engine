#include "engine/editor/world_panel.hpp"

#include "engine/core/path.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// Map cells per side at most; a wider world shows a note instead, so the map stays responsive.
constexpr std::int32_t kMapLimit = 24;
constexpr float kMapButtonWidth = 72.0f;

ImVec4 phase_colour(world::CellPhase phase) {
    switch (phase) {
        case world::CellPhase::Unloaded:
            return ImVec4(0.25f, 0.25f, 0.28f, 1.0f);
        case world::CellPhase::Reading:
        case world::CellPhase::Decoding:
            return ImVec4(0.70f, 0.60f, 0.20f, 1.0f);
        case world::CellPhase::Loaded:
            return ImVec4(0.25f, 0.45f, 0.75f, 1.0f);
        case world::CellPhase::Ready:
            return ImVec4(0.20f, 0.60f, 0.65f, 1.0f);
        case world::CellPhase::Active:
            return ImVec4(0.25f, 0.70f, 0.35f, 1.0f);
        case world::CellPhase::Failed:
            return ImVec4(0.75f, 0.25f, 0.25f, 1.0f);
    }
    return ImVec4(0.25f, 0.25f, 0.28f, 1.0f);
}

std::string_view phase_name(world::CellPhase phase) {
    switch (phase) {
        case world::CellPhase::Unloaded:
            return "Unloaded";
        case world::CellPhase::Reading:
            return "Reading";
        case world::CellPhase::Decoding:
            return "Decoding";
        case world::CellPhase::Loaded:
            return "Loaded";
        case world::CellPhase::Ready:
            return "Ready";
        case world::CellPhase::Active:
            return "Active";
        case world::CellPhase::Failed:
            return "Failed";
    }
    return "Unknown";
}

const world::WorldCell* find_cell(const world::World& world, world::CellCoord coord) {
    for (const auto& cell : world.cells) {
        if (cell.coord == coord) {
            return &cell;
        }
    }
    return nullptr;
}

}  // namespace

WorldPanel::WorldPanel() {
    constexpr std::string_view kDefaultDirectory = "world";
    std::copy(kDefaultDirectory.begin(), kDefaultDirectory.end(), path_.begin());
}

void WorldPanel::draw(bool& open) {
    if (!ImGui::Begin("World", &open)) {
        ImGui::End();
        return;
    }
    if (!editor_.has_value()) {
        toolbar();
        ImGui::TextUnformatted("Create or open a world to edit it.");
    } else {
        if (auto ticked = editor_->tick(); !ticked.ok()) {
            set_status("Streaming: " + ticked.error());
        }
        toolbar();
        ImGui::Separator();
        cell_map();
        ImGui::Separator();
        cell_controls();
        ImGui::Separator();
        object_controls();
        ImGui::Separator();
        overlay();
    }
    if (!status_.empty()) {
        ImGui::TextUnformatted(status_.c_str());
    }
    ImGui::End();
}

void WorldPanel::new_world() {
    world::WorldDesc desc;
    desc.origin = glm::vec2(origin_x_, origin_z_);
    desc.cell_size = cell_size_;
    auto path = core::Path::from_utf8(std::string_view(path_.data()));
    if (!path.ok()) {
        set_status("New: the path is not valid UTF-8.");
        return;
    }
    auto created = WorldEditor::create(desc, path.value());
    if (!created.ok()) {
        set_status("New: " + created.error());
        return;
    }
    editor_.emplace(std::move(created.value()));
    selected_.reset();
    selected_object_.reset();
    set_status("New world created. Nothing is written until Save.");
}

void WorldPanel::open_world() {
    auto path = core::Path::from_utf8(std::string_view(path_.data()));
    if (!path.ok()) {
        set_status("Open: the path is not valid UTF-8.");
        return;
    }
    auto opened = WorldEditor::open(path.value());
    if (!opened.ok()) {
        set_status("Open: " + opened.error());
        return;
    }
    editor_.emplace(std::move(opened.value()));
    selected_.reset();
    selected_object_.reset();
    set_status("Opened " + std::string(path_.data()) + ".");
}

void WorldPanel::save_world() {
    if (!editor_.has_value()) {
        return;
    }
    auto saved = editor_->save();
    if (!saved.ok()) {
        set_status("Save: " + saved.error());
        return;
    }
    set_status("Saved " + std::to_string(saved.value()) + " file(s).");
}

void WorldPanel::toolbar() {
    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("Directory", path_.data(), path_.size());
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputFloat("Origin X", &origin_x_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputFloat("Origin Z", &origin_z_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputFloat("Cell size (m)", &cell_size_);
    if (ImGui::Button("New")) {
        new_world();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        open_world();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save") && editor_.has_value()) {
        save_world();
    }
    if (editor_.has_value() && editor_->dirty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted("(unsaved)");
    }
}

void WorldPanel::cell_map() {
    const world::World& world = editor_->world();
    ImGui::TextUnformatted("Cell map (colour is residency; P pinned, S unloaded by you, * unsaved, C camera)");
    if (world.cells.empty()) {
        ImGui::TextUnformatted("No cells yet. Add an object to create one.");
        return;
    }
    std::int32_t min_x = world.cells.front().coord.x;
    std::int32_t max_x = min_x;
    std::int32_t min_z = world.cells.front().coord.z;
    std::int32_t max_z = min_z;
    for (const auto& cell : world.cells) {
        min_x = std::min(min_x, cell.coord.x);
        max_x = std::max(max_x, cell.coord.x);
        min_z = std::min(min_z, cell.coord.z);
        max_z = std::max(max_z, cell.coord.z);
    }
    if (max_x - min_x >= kMapLimit || max_z - min_z >= kMapLimit) {
        ImGui::TextUnformatted("The world is too wide for the map. Select a cell by its coordinates:");
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("Cell X", &select_x_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("Cell Z", &select_z_);
        ImGui::SameLine();
        if (ImGui::Button("Select")) {
            selected_ = world::CellCoord{select_x_, select_z_};
            selected_object_.reset();
        }
        return;
    }
    const world::CellCoord camera_cell = world::cell_coord_at(world.desc, editor_->camera());

    for (std::int32_t z = min_z; z <= max_z; ++z) {
        for (std::int32_t x = min_x; x <= max_x; ++x) {
            if (x > min_x) {
                ImGui::SameLine();
            }
            const world::CellCoord coord{x, z};
            const world::WorldCell* cell = find_cell(world, coord);
            if (cell == nullptr) {
                ImGui::Dummy(ImVec2(kMapButtonWidth, 0.0f));
                continue;
            }
            const world::CellPhase phase = editor_->phase(coord);
            std::string label = std::to_string(x) + "," + std::to_string(z);
            if (editor_->pinned(coord)) {
                label += " P";
            }
            if (editor_->suppressed(coord)) {
                label += " S";
            }
            if (cell->dirty) {
                label += " *";
            }
            if (coord == camera_cell) {
                label += " C";
            }
            const ImVec4 colour = phase_colour(phase);
            ImGui::PushStyleColor(ImGuiCol_Button, colour);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(colour.x + 0.1f, colour.y + 0.1f, colour.z + 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(colour.x + 0.2f, colour.y + 0.2f, colour.z + 0.2f, 1.0f));
            ImGui::PushID(static_cast<int>(x * 65536 + z));
            if (ImGui::Button(label.c_str(), ImVec2(kMapButtonWidth, 0.0f))) {
                selected_ = coord;
                selected_object_.reset();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Cell %d,%d: %s, %zu object(s)", x, z, std::string(phase_name(phase)).c_str(), cell->objects.size());
            }
            ImGui::PopID();
            ImGui::PopStyleColor(3);
        }
        ImGui::NewLine();
    }
}

void WorldPanel::cell_controls() {
    if (!selected_.has_value()) {
        ImGui::TextUnformatted("Select a cell on the map.");
        return;
    }
    const world::CellCoord coord = *selected_;
    const world::WorldCell* cell = find_cell(editor_->world(), coord);
    if (cell == nullptr) {
        selected_.reset();
        return;
    }
    ImGui::Text("Cell %d,%d: %s  objects %zu  %s", coord.x, coord.z, std::string(phase_name(editor_->phase(coord))).c_str(),
                cell->objects.size(), cell->dirty ? "unsaved changes (kept in memory)" : "clean");
    if (ImGui::Button("Load")) {
        editor_->load_cell(coord);
    }
    ImGui::SameLine();
    bool pinned = editor_->pinned(coord);
    if (ImGui::Checkbox("Pin", &pinned)) {
        editor_->set_pinned(coord, pinned);
    }
    ImGui::SameLine();
    if (ImGui::Button("Unload")) {
        editor_->unload_cell(coord);
    }
    ImGui::SameLine();
    if (ImGui::Button("Go to cell")) {
        editor_->go_to_cell(coord);
    }
}

void WorldPanel::object_controls() {
    if (!selected_.has_value()) {
        return;
    }
    const world::World& world = editor_->world();
    const world::WorldCell* cell = find_cell(world, *selected_);
    if (cell == nullptr) {
        return;
    }
    ImGui::TextUnformatted("Objects in the selected cell");
    for (const auto& object : cell->objects) {
        const bool chosen = selected_object_.has_value() && *selected_object_ == object.id;
        const std::string label = (object.name.empty() ? std::string("(unnamed)") : object.name) + "##" + object.id.to_string();
        if (ImGui::Selectable(label.c_str(), chosen)) {
            selected_object_ = object.id;
            move_position_ = {object.position.x, object.position.y, object.position.z};
        }
    }
    if (selected_object_.has_value()) {
        ImGui::DragFloat3("Position", move_position_.data(), 0.25f);
        ImGui::SameLine();
        if (ImGui::Button("Move")) {
            const glm::vec3 position(move_position_[0], move_position_[1], move_position_[2]);
            if (auto moved = editor_->move_object(*selected_object_, position); !moved.ok()) {
                set_status("Move: " + moved.error());
            } else {
                set_status("Moved. The object keeps its GUID and references.");
            }
        }
    }
    add_object_controls();
}

void WorldPanel::add_object_controls() {
    ImGui::TextUnformatted("Add an object (its cell is the one that owns its position)");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("Name", object_name_.data(), object_name_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputText("Asset GUID", asset_text_.data(), asset_text_.size());
    ImGui::SetNextItemWidth(240.0f);
    ImGui::DragFloat3("Add position", add_position_.data(), 0.25f);
    ImGui::SameLine();
    ImGui::Checkbox("Always loaded", &add_always_loaded_);
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
        auto asset = core::Guid::parse(std::string_view(asset_text_.data()));
        if (!asset.ok()) {
            set_status("Add: the asset is not a GUID.");
            return;
        }
        world::WorldObject object;
        object.id = core::Guid::generate();
        object.name = std::string(object_name_.data());
        object.asset = asset.value();
        object.position = glm::vec3(add_position_[0], add_position_[1], add_position_[2]);
        if (auto added = editor_->add_object(std::move(object), add_always_loaded_); !added.ok()) {
            set_status("Add: " + added.error());
        } else {
            set_status("Object added.");
        }
    }
}

void WorldPanel::overlay() {
    const world::StreamingBudget& budget = editor_->budget();
    const world::StreamingWindow& window = editor_->window();
    const world::StreamingStats& stats = editor_->stats();
    const glm::dvec2 camera = editor_->camera();

    ImGui::TextUnformatted("Streaming overlay");
    const world::CellCoord camera_cell = world::cell_coord_at(editor_->world().desc, camera);
    ImGui::Text("Camera %.1f, %.1f (cell %d, %d)", camera.x, camera.y, camera_cell.x, camera_cell.z);
    double camera_x = camera.x;
    double camera_z = camera.y;
    ImGui::SetNextItemWidth(160.0f);
    const bool x_changed = ImGui::InputDouble("Camera X", &camera_x);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const bool z_changed = ImGui::InputDouble("Camera Z", &camera_z);
    if (x_changed || z_changed) {
        editor_->set_camera(glm::dvec2(camera_x, camera_z));
    }
    ImGui::Text("Window: active %d, prefetch %d cells", window.active_radius, window.prefetch_radius);
    ImGui::Text("Budget per update: IO %zu, decode %zu, GPU %zu bytes", budget.io_bytes_per_update, budget.decode_bytes_per_update,
                budget.gpu_upload_bytes_per_update);
    ImGui::Text("Residency: active %zu, resident %zu (peak %zu), CPU %zu bytes (peak %zu)", stats.active_cells, stats.resident_cells,
                stats.peak_resident_cells, stats.resident_cpu_bytes, stats.peak_resident_cpu_bytes);
    ImGui::Text("GPU %zu bytes (peak %zu), transient %zu bytes (peak %zu)", stats.gpu_bytes, stats.peak_gpu_bytes, stats.transient_bytes,
                stats.peak_transient_bytes);
    ImGui::Text("Loads %zu, uploads %zu, activations %zu, deactivations %zu, unloads %zu", stats.loads, stats.uploads, stats.activations,
                stats.deactivations, stats.unloads);
    ImGui::Text("Failed %zu, cancelled %zu, teleports %zu", stats.failed_cells, stats.cancelled_requests, stats.teleports);
}

void WorldPanel::set_status(std::string text) {
    status_ = std::move(text);
}

}  // namespace engine::editor
