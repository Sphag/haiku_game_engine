#pragma once

// The editable animation state graph (US-070): the document a visual editor works on. It holds the state graph (US-069), the skeleton
// its clips are checked against, the clip library, and an undo/redo history. It needs no ImGui or device, so the editor's panel is a
// thin view over it.
//
// Rules of the document:
//   - Every edit is checked as a whole before it is applied. The candidate graph goes through StateMachine::create, the same check a
//     runtime machine gets, so an edit that would produce a graph the runtime refuses is refused here with that message and changes
//     nothing. The document is therefore always a valid, runnable graph.
//   - Compatibility is part of that check. A clip is valid for a skeleton only when its tracks match the skeleton's joints (validate_clip),
//     so choosing a clip or a skeleton that does not fit an existing state is refused and names the state.
//   - Undo and redo restore the graph and the skeleton together. The clip library is the content the document was opened with and is
//     not edited here.
//   - Selection is not part of the history. A transition is named by its index in graph().transitions.

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::animation {

// What an edit returns: nothing on success, or the reason it was refused.
using GraphEditResult = core::Result<core::Unit, std::string>;

class AnimationGraphDocument {
public:
    // A new document with one state, named "default", that plays the first clip of the library valid for the skeleton. A library with no
    // compatible clip is refused, because a graph needs a default state.
    [[nodiscard]] static core::Result<AnimationGraphDocument, std::string> create(Skeleton skeleton, ClipLibrary clips);

    // Opens a graph JSON (state_graph.hpp) against a skeleton and clip library. The text must parse, and every state's clip must be in the
    // library and valid for the skeleton. A refusal names the state or clip at fault.
    [[nodiscard]] static core::Result<AnimationGraphDocument, std::string> open(std::string_view graph_json, Skeleton skeleton,
                                                                                ClipLibrary clips);

    [[nodiscard]] const StateGraph& graph() const noexcept {
        return state_.graph;
    }
    [[nodiscard]] const Skeleton& skeleton() const noexcept {
        return state_.skeleton;
    }
    [[nodiscard]] const ClipLibrary& clips() const noexcept {
        return clips_;
    }
    // Names of the library's clips that are valid for the current skeleton, in library order. A clip picker offers only these.
    [[nodiscard]] std::vector<std::string> compatible_clips() const;
    // Counts every applied edit, undo, and redo. A view compares it to know when to read the graph again.
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }

    // States. A state name is unique and non-empty. The default state cannot be removed; choose another default first.
    [[nodiscard]] GraphEditResult add_state(const std::string& name, const std::string& clip);
    [[nodiscard]] GraphEditResult remove_state(const std::string& name);
    [[nodiscard]] GraphEditResult rename_state(const std::string& from, const std::string& to);
    [[nodiscard]] GraphEditResult set_state_clip(const std::string& name, const std::string& clip);
    [[nodiscard]] GraphEditResult set_state_speed(const std::string& name, float speed);
    [[nodiscard]] GraphEditResult set_default_state(const std::string& name);

    // Parameters. A parameter that a condition tests cannot be removed.
    [[nodiscard]] GraphEditResult add_parameter(const ParamDecl& parameter);
    [[nodiscard]] GraphEditResult remove_parameter(const std::string& name);

    // Transitions. add_transition returns the new transition's index. Its priority is one above the highest priority leaving `from`, and its
    // duration is a quarter second until it is set.
    [[nodiscard]] core::Result<std::size_t, std::string> add_transition(const std::string& from, const std::string& to);
    [[nodiscard]] GraphEditResult remove_transition(std::size_t index);
    [[nodiscard]] GraphEditResult set_transition_timing(std::size_t index, float duration, int priority);
    [[nodiscard]] GraphEditResult add_condition(std::size_t transition, const Condition& condition);
    [[nodiscard]] GraphEditResult set_condition(std::size_t transition, std::size_t condition_index, const Condition& condition);
    [[nodiscard]] GraphEditResult remove_condition(std::size_t transition, std::size_t condition_index);

    // Changes the skeleton. Refused when a state's clip is not valid for the new skeleton.
    [[nodiscard]] GraphEditResult set_skeleton(Skeleton skeleton);

    [[nodiscard]] bool can_undo() const noexcept {
        return !undo_.empty();
    }
    [[nodiscard]] bool can_redo() const noexcept {
        return !redo_.empty();
    }
    bool undo();
    bool redo();

    // The graph JSON. It is valid by construction, so this cannot fail.
    [[nodiscard]] std::string save() const;

    // A runnable machine for the current graph, for previewing it. Each call makes a new machine at its default state.
    [[nodiscard]] core::Result<StateMachine, std::string> preview() const;

private:
    struct State {
        StateGraph graph;
        Skeleton skeleton;
    };

    AnimationGraphDocument(State state, ClipLibrary clips);

    // Checks `next` as a whole and makes it current, recording the previous state for undo. Clears redo on success.
    [[nodiscard]] GraphEditResult commit(State next);

    State state_;
    ClipLibrary clips_;
    std::vector<State> undo_;
    std::vector<State> redo_;
    std::uint64_t revision_ = 0;
};

}  // namespace engine::animation
