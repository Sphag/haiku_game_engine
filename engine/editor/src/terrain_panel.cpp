#include "engine/editor/terrain_panel.hpp"

#include "engine/core/path.hpp"
#include "engine/world/terrain.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

constexpr std::array<const char*, 5> kBrushNames = {"Raise", "Lower", "Smooth", "Flatten", "Paint"};
constexpr std::array<std::uint32_t, 5> kTileCellChoices = {16, 32, 64, 128, 256};
constexpr std::array<const char*, 5> kTileCellNames = {"16", "32", "64", "128", "256"};
// Map colours of the four layers, before height shading.
constexpr std::array<glm::vec3, world::kTerrainLayers> kLayerColours = {
    glm::vec3(0.42f, 0.55f, 0.32f),
    glm::vec3(0.55f, 0.45f, 0.30f),
    glm::vec3(0.50f, 0.50f, 0.52f),
    glm::vec3(0.85f, 0.80f, 0.55f),
};
// The map draws at most this many samples per side; a coarser stride keeps large terrains responsive.
constexpr std::uint32_t kMapSamples = 256;

}  // namespace

void TerrainPanel::draw(bool& open) {
    if (!ImGui::Begin("Terrain", &open)) {
        ImGui::End();
        if (!open) {
            finish_stroke();
        }
        return;
    }
    if (!editor_.has_value()) {
        new_terrain();
    }
    toolbar();
    ImGui::Separator();
    brush_controls();
    ImGui::Separator();
    if (editor_.has_value()) {
        map_area();
    }
    if (!status_.empty()) {
        ImGui::TextUnformatted(status_.c_str());
    }
    ImGui::End();
    if (!open) {
        // The window closed inside a stroke, so no release will reach the map. The stroke ends here, as one command.
        finish_stroke();
    }
}

void TerrainPanel::new_terrain() {
    world::TerrainDesc desc;
    desc.tiles_x = static_cast<std::uint32_t>(std::clamp(tiles_x_, 1, 32));
    desc.tiles_z = static_cast<std::uint32_t>(std::clamp(tiles_z_, 1, 32));
    desc.tile_cells = kTileCellChoices[static_cast<std::size_t>(tile_cells_index_)];
    desc.cell_size = 1.0f;
    auto created = TerrainEditor::create(desc);
    if (!created.ok()) {
        set_status("New terrain: " + created.error());
        return;
    }
    editor_.emplace(std::move(created.value()));
    set_status("New terrain created.");
}

void TerrainPanel::open_file() {
    auto path = core::Path::from_utf8(std::string_view(path_.data()));
    if (!path.ok()) {
        set_status("Open: the path is not valid UTF-8.");
        return;
    }
    auto opened = TerrainEditor::open(path.value());
    if (!opened.ok()) {
        set_status("Open: " + opened.error());
        return;
    }
    editor_.emplace(std::move(opened.value()));
    set_status("Opened " + std::string(path_.data()) + ".");
}

void TerrainPanel::save_file() {
    if (!editor_.has_value()) {
        return;
    }
    auto path = core::Path::from_utf8(std::string_view(path_.data()));
    if (!path.ok()) {
        set_status("Save: the path is not valid UTF-8.");
        return;
    }
    auto saved = editor_->save(path.value());
    if (!saved.ok()) {
        set_status("Save: " + saved.error());
        return;
    }
    set_status("Saved " + std::string(path_.data()) + ".");
}

void TerrainPanel::toolbar() {
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("Tiles X", &tiles_x_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("Tiles Z", &tiles_z_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::Combo("Tile cells", &tile_cells_index_, kTileCellNames.data(), static_cast<int>(kTileCellNames.size()));
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        finish_stroke();
        new_terrain();
    }

    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("Path", path_.data(), path_.size());
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        finish_stroke();
        open_file();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        finish_stroke();
        save_file();
    }

    const bool can_edit = editor_.has_value();
    ImGui::BeginDisabled(!can_edit || !editor_->document().can_undo() || stroking_);
    if (ImGui::Button("Undo")) {
        editor_->undo();
        after_edit();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_edit || !editor_->document().can_redo() || stroking_);
    if (ImGui::Button("Redo")) {
        editor_->redo();
        after_edit();
    }
    ImGui::EndDisabled();
    if (can_edit && editor_->document().dirty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted("(unsaved)");
    }
}

void TerrainPanel::brush_controls() {
    if (!editor_.has_value()) {
        ImGui::TextUnformatted("Create or open a terrain to edit it.");
        return;
    }
    world::Brush& brush = editor_->brush();
    int kind = static_cast<int>(brush.kind);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Brush", &kind, kBrushNames.data(), static_cast<int>(kBrushNames.size()))) {
        brush.kind = static_cast<world::BrushKind>(kind);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Radius (m)", &brush.radius, 0.1f, 64.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Strength", &brush.strength, 0.01f, 1.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Falloff", &brush.falloff, 0.0f, 1.0f);
    if (brush.kind == world::BrushKind::Paint) {
        int layer = static_cast<int>(brush.layer);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("Layer", &layer, "Layer 0\0Layer 1\0Layer 2\0Layer 3\0\0")) {
            brush.layer = static_cast<std::uint32_t>(layer);
        }
    }
    if (brush.kind == world::BrushKind::Flatten) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Target height (m)", &brush.target_height, -100.0f, 100.0f);
    }
}

void TerrainPanel::map_area() {
    const world::Terrain& terrain = editor_->terrain();
    const glm::vec2 extent = world::terrain_extent(terrain);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float scale = std::max(0.01f, std::min(avail.x / extent.x, std::max(avail.y, 64.0f) / extent.y));
    const ImVec2 size(extent.x * scale, extent.y * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##terrain_map", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Shade the map by height over the sampled grid, and tint each sample by its layer weights.
    const std::uint32_t stride = std::max<std::uint32_t>(
        1, (std::max(terrain.samples_x, terrain.samples_z) + kMapSamples - 1) / kMapSamples);
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (std::uint32_t iz = 0; iz < terrain.samples_z; iz += stride) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ix += stride) {
            const float h = world::sample_height(terrain, ix, iz);
            low = std::min(low, h);
            high = std::max(high, h);
        }
    }
    const float range = high - low;
    const float cell_px = scale * terrain.desc.cell_size * static_cast<float>(stride);
    for (std::uint32_t iz = 0; iz < terrain.samples_z; iz += stride) {
        for (std::uint32_t ix = 0; ix < terrain.samples_x; ix += stride) {
            const glm::vec4 splat = world::sample_splat(terrain, ix, iz);
            glm::vec3 colour(0.0f);
            for (std::size_t layer = 0; layer < world::kTerrainLayers; ++layer) {
                colour += kLayerColours[layer] * splat[static_cast<int>(layer)];
            }
            const float shade = range > 0.0f ? (world::sample_height(terrain, ix, iz) - low) / range : 0.5f;
            const float light = 0.35f + 0.65f * shade;
            colour *= light;
            const ImVec2 p0(origin.x + static_cast<float>(ix) * terrain.desc.cell_size * scale, origin.y + static_cast<float>(iz) * terrain.desc.cell_size * scale);
            draw->AddRectFilled(p0, ImVec2(p0.x + cell_px, p0.y + cell_px),
                                IM_COL32(static_cast<int>(colour.r * 255.0f), static_cast<int>(colour.g * 255.0f),
                                         static_cast<int>(colour.b * 255.0f), 255));
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 pointer_world((io.MousePos.x - origin.x) / scale, (io.MousePos.y - origin.y) / scale);
    if (hovered) {
        draw->AddCircle(io.MousePos, editor_->brush().radius * scale, IM_COL32(255, 255, 255, 200), 0, 1.5f);
    }

    if (ImGui::IsItemActivated() && !stroking_) {
        auto begun = editor_->begin_stroke();
        if (begun.ok()) {
            stroking_ = true;
        } else {
            set_status("Stroke: " + begun.error());
        }
    }
    if (stroking_ && ImGui::IsItemActive()) {
        auto stamped = editor_->stamp(pointer_world);
        if (!stamped.ok()) {
            set_status("Stamp: " + stamped.error());
            finish_stroke();
        }
    }
    if (ImGui::IsItemDeactivated()) {
        finish_stroke();
    }
}

void TerrainPanel::finish_stroke() {
    if (!stroking_) {
        return;
    }
    stroking_ = false;
    auto ended = editor_->end_stroke();
    if (!ended.ok()) {
        set_status("Stroke: " + ended.error());
        return;
    }
    after_edit();
}

void TerrainPanel::after_edit() {
    if (!editor_.has_value()) {
        return;
    }
    // The editor has no physics world, so only the LOD meshes are invalidated here. A body created later is refreshed on its next call.
    const TerrainRefresh report = editor_->refresh(nullptr);
    set_status("Invalidated " + std::to_string(report.invalidated.size()) + " tile(s); their meshes rebuild on next use.");
}

void TerrainPanel::set_status(std::string text) {
    status_ = std::move(text);
}

}  // namespace engine::editor
