#pragma once

// The world window (US-072): a new or opened world directory, a cell map coloured by residency state, the load, pin, and unload controls
// for the selected cell, navigation of the editor camera to a cell, object moves between cells, and the streaming overlay (budgets and
// residency). The WorldEditor holds the state; this class draws it and turns clicks into commands.

#include "engine/core/guid.hpp"
#include "engine/editor/world_editor.hpp"
#include "engine/world/partition.hpp"

#include <array>
#include <optional>
#include <string>

namespace engine::editor {

class WorldPanel {
public:
    WorldPanel();

    // Draws the window while `open` is set, and advances residency once per frame. The window's close button clears it.
    void draw(bool& open);

private:
    void new_world();
    void open_world();
    void save_world();
    void toolbar();
    void cell_map();
    void cell_controls();
    void object_controls();
    void add_object_controls();
    void overlay();
    void set_status(std::string text);

    std::optional<WorldEditor> editor_;
    std::array<char, 512> path_{};
    float origin_x_ = 0.0f;
    float origin_z_ = 0.0f;
    float cell_size_ = 256.0f;
    std::optional<world::CellCoord> selected_;
    std::optional<core::Guid> selected_object_;
    int select_x_ = 0;  // the coordinates typed when the world is too wide for the map
    int select_z_ = 0;
    std::array<float, 3> move_position_{};
    std::array<char, 128> object_name_{};
    std::array<char, 64> asset_text_{};
    std::array<float, 3> add_position_{};
    bool add_always_loaded_ = false;
    std::string status_;
};

}  // namespace engine::editor
