#pragma once

// The material graph window (US-068): a node canvas over a GraphDocument, a properties column, a sphere preview, and the error list.
// The document holds every piece of state that matters (graph, layout, selection, history, last valid version); this class draws it
// and turns mouse and keyboard input into document edits. The layout is saved beside the graph as "<path>.layout.json".

#include "engine/material_graph/edit.hpp"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace engine::editor {

class MaterialGraphPanel {
public:
    // Draws the window while `open` is set. The window's close button clears it.
    void draw(bool& open);

private:
    void toolbar();
    void canvas(float width, float height);
    void side_column(float width, float height);
    void properties();
    void sphere_preview(float size);
    void error_list();
    void open_file();
    void save_file();
    void set_status(std::string text);

    std::optional<material_graph::GraphDocument> document_;
    std::array<char, 512> path_{};
    std::array<char, 128> texture_name_{};
    std::string status_;
    std::size_t add_kind_ = 0;

    ImVec2 pan_{0.0f, 0.0f};
    bool moving_ = false;
    ImVec2 move_offset_{0.0f, 0.0f};
    bool linking_ = false;
    std::string link_source_;

    // The sphere is sampled once per document revision, because a sample runs every texel through the evaluator.
    std::uint64_t preview_revision_ = 0;
    bool preview_sampled_ = false;
    material_graph::PreviewSwatch swatch_;
    std::string preview_message_;
};

}  // namespace engine::editor
