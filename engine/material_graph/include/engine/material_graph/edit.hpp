#pragma once

// The editable material graph (US-068): the document a visual editor works on. It holds the graph (US-067), the node layout, the
// selection, and an undo/redo history, and it needs no ImGui or device, so the editor's canvas is a thin view over it.
//
// Two rules keep the cooked side independent of the editor:
//   - The layout is a separate document (engine.material_graph.layout). It is never part of the graph, so moving a node changes
//     neither the generated source nor any cache key. The cooked runtime reads only the graph.
//   - Edits that break the graph are accepted, so an author can work through a broken state, and the errors name the nodes. The
//     last version that analyzed clean is kept (last_valid()), and the preview draws it. save() refuses a broken graph, so the file
//     on disk is always a valid graph.
//
// Edits that cannot describe a graph at all (an unknown node, a pin the kind does not have, a self link, removing the output) are
// refused with a message and change nothing.

#include "engine/core/result.hpp"
#include "engine/material_graph/compile.hpp"
#include "engine/material_graph/graph.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace engine::material_graph {

inline constexpr std::string_view kLayoutSchemaName = "engine.material_graph.layout";

// The top-left corner of each node on the canvas, in canvas units, by node id.
using Layout = std::map<std::string, glm::vec2, std::less<>>;

// The result of a preview: a square grid of the surface's base colour and roughness over UV in [0, 1]. Row 0 is v = 0.
struct PreviewSwatch {
    std::size_t size = 0;
    std::vector<glm::vec4> base_color;  // size * size, row-major
    std::vector<float> roughness;       // size * size, row-major
};

// What an edit returns: nothing on success, or the reason it was refused.
using EditResult = core::Result<core::Unit, std::string>;

class GraphDocument {
public:
    // An empty document with one PBR output. The output is the only node a document must keep.
    [[nodiscard]] static GraphDocument create(std::string name);

    // Opens a graph and, optionally, its layout. A missing or empty layout places the nodes on a grid in graph order. A layout entry
    // for an unknown node is dropped. The graph is checked by parse_graph, so a graph that breaks a rule is refused here with the
    // rule's errors, and an opened document is always valid.
    [[nodiscard]] static core::Result<GraphDocument, std::vector<GraphError>> open(std::string_view graph_json, std::string_view layout_json);

    [[nodiscard]] const Graph& graph() const noexcept { return state_.graph; }
    [[nodiscard]] const Layout& layout() const noexcept { return state_.layout; }
    [[nodiscard]] bool valid() const noexcept { return errors_.empty(); }
    // Counts every change to the graph or the layout, including undo and redo. A view compares it to know when to sample again.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const std::vector<GraphError>& errors() const noexcept { return errors_; }
    // The graph as of the last edit that analyzed clean. Before any valid edit it is the graph the document opened with (or the
    // default graph, which is valid).
    [[nodiscard]] const Graph& last_valid() const noexcept { return last_valid_; }
    // Whether a node has at least one error that names it. Nodes with graph-level errors only are not in this set.
    [[nodiscard]] bool node_has_error(std::string_view id) const;

    // Node and graph edits. Each returns the error message of a refused edit, or nothing when it was applied.
    [[nodiscard]] core::Result<std::string, std::string> add_node(NodeKind kind, glm::vec2 position);
    [[nodiscard]] EditResult remove_nodes(const std::vector<std::string>& ids);
    [[nodiscard]] EditResult link(const std::string& consumer, const std::string& pin, const std::string& source);
    [[nodiscard]] EditResult unlink(const std::string& consumer, const std::string& pin);
    [[nodiscard]] EditResult set_value(const std::string& id, glm::vec4 value);
    [[nodiscard]] EditResult set_type(const std::string& id, ValueType type);
    [[nodiscard]] EditResult set_name(const std::string& id, const std::string& name);
    [[nodiscard]] EditResult set_op(const std::string& id, ArithmeticOp op);
    [[nodiscard]] EditResult add_texture(TextureDecl texture);
    // Moves each listed node by the same offset. The move is one history entry.
    [[nodiscard]] EditResult move_nodes(const std::vector<std::string>& ids, glm::vec2 offset);

    // Undo and redo. Selection is not part of the history.
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    bool undo();
    bool redo();

    // Selection: a set of node ids. Ids that are not in the graph are dropped when the graph changes.
    [[nodiscard]] const std::set<std::string, std::less<>>& selection() const noexcept { return selection_; }
    void select(const std::string& id, bool additive);
    void clear_selection() { selection_.clear(); }

    // Saving. The graph is written only when it is valid; the layout is always written.
    [[nodiscard]] core::Result<std::string, std::vector<GraphError>> save_graph() const;
    [[nodiscard]] std::string save_layout() const;

    // Samples the last valid graph on a size x size grid. `overrides` replaces parameter values for this preview only (the same
    // shape as ParameterValues). Fails with the graph's errors when the last valid graph cannot be evaluated.
    [[nodiscard]] core::Result<PreviewSwatch, std::vector<GraphError>> preview(std::size_t size, const ParameterValues& overrides = {}) const;

private:
    struct State {
        Graph graph;
        Layout layout;
    };

    GraphDocument(State state, std::vector<GraphError> errors, Graph last_valid);

    // Applies `change` to the node `id` when its kind is one of `kinds`, and commits the result as one history entry.
    template <typename Change>
    EditResult change_node(const std::string& id, std::initializer_list<NodeKind> kinds, const char* what, Change change);
    [[nodiscard]] std::size_t index_of(const std::string& id) const;

    // Replaces the state and records the previous one for undo. Clears redo, analyzes the graph, and keeps the selection to live ids.
    void commit(State next);
    void revalidate();
    [[nodiscard]] Node* find(const std::string& id);
    [[nodiscard]] const Node* find(const std::string& id) const;
    [[nodiscard]] std::string fresh_id(std::string_view prefix) const;

    State state_;
    std::vector<GraphError> errors_;
    Graph last_valid_;
    std::vector<State> undo_;
    std::vector<State> redo_;
    std::set<std::string, std::less<>> selection_;
    std::uint64_t revision_ = 0;
};

}  // namespace engine::material_graph
