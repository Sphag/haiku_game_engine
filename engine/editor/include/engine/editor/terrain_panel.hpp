#pragma once

// The terrain window (US-071): a new or opened terrain, the brush settings, undo and redo, and a top-down map. Pressing the left button on
// the map starts a stroke; the stroke stamps the brush under the pointer each frame and ends on release, so it is one undo command. The
// map is the brush surface, so the window needs no 3D picking. The TerrainEditor holds the state; this class only draws it and turns
// the pointer into stamps. Heights are shaded on the map and each layer has a colour; the viewport does not draw terrain yet.

#include "engine/editor/terrain_editor.hpp"

#include <array>
#include <optional>
#include <string>

namespace engine::editor {

class TerrainPanel {
public:
    // Draws the window while `open` is set. The window's close button clears it.
    void draw(bool& open);

private:
    void new_terrain();
    void open_file();
    void save_file();
    void brush_controls();
    void toolbar();
    void map_area();
    void finish_stroke();
    void after_edit();
    void set_status(std::string text);

    std::optional<TerrainEditor> editor_;
    std::array<char, 512> path_{};
    int tiles_x_ = 2;
    int tiles_z_ = 2;
    int tile_cells_index_ = 2;  // into kTileCellChoices
    std::string status_;
    bool stroking_ = false;
};

}  // namespace engine::editor
