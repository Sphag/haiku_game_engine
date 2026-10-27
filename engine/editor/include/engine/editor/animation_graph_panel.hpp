#pragma once

// The animation graph window (US-070): a states and transitions editor over an AnimationGraphDocument, a preview machine that runs the
// edited graph, and a Play-mode parameter monitor. The document holds every piece of state that matters (graph, skeleton, history); this
// class draws it and turns widget input into document edits. Edits that the document refuses show its message in the status line.
//
// Content. This build has no clip importer, so the window opens graphs against the built-in demo rig (a two-joint skeleton with idle,
// walk, and jump clips). The graph file format is the state graph's (state_graph.hpp), saved at the path typed in the toolbar.

#include "engine/animation/graph_edit.hpp"
#include "engine/animation/state_machine.hpp"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace engine::editor {

class AnimationGraphPanel {
public:
    // Draws the window while `open` is set; the window's close button clears it. `play_animators` is the live Play session's set, or null
    // when no session runs; the parameter monitor reads the machine named in its field from it.
    void draw(bool& open, animation::AnimatorSet* play_animators);

private:
    void toolbar();
    void states_column();
    void transitions_column();
    void preview();
    void play_monitor(animation::AnimatorSet* play_animators);
    void condition_row(std::size_t transition, std::size_t index, const animation::Condition& condition);
    void open_file();
    void save_file();
    // Returns whether the edit was applied. A refused edit leaves its message in the status line.
    bool applied(const animation::GraphEditResult& result);
    void set_status(std::string text);

    std::optional<animation::AnimationGraphDocument> document_;
    std::array<char, 512> path_{};
    std::array<char, 64> new_state_name_{};
    std::array<char, 64> new_parameter_name_{};
    std::array<char, 64> monitor_name_{};
    std::string status_;

    std::size_t state_ = 0;
    std::size_t parameter_type_ = 0;
    std::size_t transition_ = 0;
    std::size_t from_ = 0;
    std::size_t to_ = 0;
    std::size_t clip_choice_ = 0;

    // The preview machine is rebuilt when the document changes, so it always runs the current graph.
    std::optional<animation::StateMachine> preview_;
    std::uint64_t preview_revision_ = 0;
    bool playing_ = true;
};

}  // namespace engine::editor
