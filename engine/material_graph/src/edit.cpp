#include "engine/material_graph/edit.hpp"

#include "engine/material_graph/evaluate.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

namespace engine::material_graph {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kHistoryLimit = 256;
constexpr float kGridColumnWidth = 240.0f;
constexpr float kGridRowHeight = 160.0f;
constexpr std::size_t kGridColumns = 4;
constexpr std::size_t kNoNode = std::numeric_limits<std::size_t>::max();

glm::vec2 grid_position(std::size_t index) {
    return glm::vec2(static_cast<float>(index % kGridColumns) * kGridColumnWidth,
                     static_cast<float>(index / kGridColumns) * kGridRowHeight);
}

// Reads a layout document. Entries for nodes the graph does not have are dropped, because a layout outlives an edit that removed
// its node.
core::Result<Layout, std::string> parse_layout(std::string_view json, const Graph& graph) {
    const Json doc = Json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return core::failure(std::string("layout is not a JSON object"));
    }
    if (!doc.contains("schema") || !doc.at("schema").is_string() || doc.at("schema").get<std::string>() != kLayoutSchemaName) {
        return core::failure(std::string("layout schema is not '") + std::string(kLayoutSchemaName) + "'");
    }
    if (!doc.contains("version") || !doc.at("version").is_number_integer() || doc.at("version").get<std::int64_t>() != kFormatVersion) {
        return core::failure(std::string("layout version is not ") + std::to_string(kFormatVersion));
    }
    if (!doc.contains("nodes") || !doc.at("nodes").is_object()) {
        return core::failure(std::string("layout has no 'nodes' object"));
    }

    Layout layout;
    for (const auto& entry : doc.at("nodes").items()) {
        const std::string id = entry.key();
        const Json& position = entry.value();
        if (!position.is_array() || position.size() != 2 || !position[0].is_number() || !position[1].is_number()) {
            return core::failure("layout position of '" + id + "' is not [x, y]");
        }
        const bool known = std::any_of(graph.nodes.begin(), graph.nodes.end(), [wanted = id](const Node& node) { return node.id == wanted; });
        if (known) {
            layout[id] = glm::vec2(position[0].get<float>(), position[1].get<float>());
        }
    }
    return core::success(std::move(layout));
}

std::string layout_json(const Layout& layout) {
    Json nodes = Json::object();
    for (const auto& [id, position] : layout) {
        nodes[id] = Json::array({position.x, position.y});
    }
    Json doc;
    doc["schema"] = std::string(kLayoutSchemaName);
    doc["version"] = kFormatVersion;
    doc["nodes"] = nodes;
    return doc.dump(2) + "\n";
}

GraphError document_error(std::string message) {
    return GraphError{.node = {}, .kind = {}, .message = std::move(message)};
}

// Whether `kind` has an input pin called `pin`.
bool has_pin(NodeKind kind, const std::string& pin) {
    const std::vector<PinSpec> specs = pin_specs(kind);
    return std::any_of(specs.begin(), specs.end(), [&pin](const PinSpec& spec) { return spec.name == pin; });
}

EditResult refuse(std::string message) {
    return core::failure(std::move(message));
}

}  // namespace

GraphDocument::GraphDocument(State state, std::vector<GraphError> errors, Graph last_valid)
    : state_(std::move(state)), errors_(std::move(errors)), last_valid_(std::move(last_valid)) {}

GraphDocument GraphDocument::create(std::string name) {
    State state;
    state.graph.name = std::move(name);

    Node output;
    output.id = "output";
    output.kind = NodeKind::PbrOutput;
    output.inputs["base_color"] = "base_color";

    Node base;
    base.id = "base_color";
    base.kind = NodeKind::Constant;
    base.type = ValueType::Vec4;
    base.value = glm::vec4(1.0f);

    state.graph.nodes = {output, base};
    state.layout["output"] = glm::vec2(kGridColumnWidth, 0.0f);
    state.layout["base_color"] = glm::vec2(0.0f, 0.0f);

    Analysis analysis = analyze(state.graph);
    Graph valid = state.graph;
    return GraphDocument(std::move(state), std::move(analysis.errors), std::move(valid));
}

core::Result<GraphDocument, std::vector<GraphError>> GraphDocument::open(std::string_view graph_json, std::string_view layout_text) {
    core::Result<Graph, std::vector<GraphError>> parsed = parse_graph(graph_json);
    if (!parsed.ok()) {
        return core::failure(parsed.error());
    }
    Graph graph = std::move(parsed).value();

    Layout layout;
    if (!layout_text.empty()) {
        core::Result<Layout, std::string> read = parse_layout(layout_text, graph);
        if (!read.ok()) {
            return core::failure(std::vector<GraphError>{document_error(read.error())});
        }
        layout = std::move(read).value();
    }
    // A node without a stored position goes to the grid, after the stored ones, in graph order.
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (!layout.contains(graph.nodes[i].id)) {
            layout[graph.nodes[i].id] = grid_position(i);
        }
    }

    // parse_graph checks every graph rule, so an opened graph is valid and is its own last valid version.
    Graph valid = graph;
    return core::success(GraphDocument(State{std::move(graph), std::move(layout)}, {}, std::move(valid)));
}

bool GraphDocument::node_has_error(std::string_view id) const {
    return std::any_of(errors_.begin(), errors_.end(), [id](const GraphError& error) { return error.node == id; });
}

std::size_t GraphDocument::index_of(const std::string& id) const {
    for (std::size_t i = 0; i < state_.graph.nodes.size(); ++i) {
        if (state_.graph.nodes[i].id == id) {
            return i;
        }
    }
    return kNoNode;
}

Node* GraphDocument::find(const std::string& id) {
    const std::size_t index = index_of(id);
    return index == kNoNode ? nullptr : &state_.graph.nodes[index];
}

const Node* GraphDocument::find(const std::string& id) const {
    const std::size_t index = index_of(id);
    return index == kNoNode ? nullptr : &state_.graph.nodes[index];
}

std::string GraphDocument::fresh_id(std::string_view prefix) const {
    for (std::size_t n = state_.graph.nodes.size() + 1;; ++n) {
        std::string candidate = std::string(prefix) + "_" + std::to_string(n);
        if (find(candidate) == nullptr) {
            return candidate;
        }
    }
}

void GraphDocument::commit(State next) {
    undo_.push_back(std::move(state_));
    if (undo_.size() > kHistoryLimit) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    state_ = std::move(next);
    ++revision_;
    revalidate();
}

void GraphDocument::revalidate() {
    Analysis analysis = analyze(state_.graph);
    errors_ = std::move(analysis.errors);
    if (errors_.empty()) {
        last_valid_ = state_.graph;
    }
    for (auto it = selection_.begin(); it != selection_.end();) {
        it = find(*it) == nullptr ? selection_.erase(it) : std::next(it);
    }
}

core::Result<std::string, std::string> GraphDocument::add_node(NodeKind kind, glm::vec2 position) {
    if (kind == NodeKind::PbrOutput) {
        return core::failure(std::string("a graph has one output, and it is already there"));
    }
    Node node;
    node.kind = kind;
    node.id = fresh_id(to_string(kind));
    if (kind == NodeKind::Parameter) {
        node.name = fresh_id("parameter");
    }
    if (kind == NodeKind::TextureSample) {
        if (state_.graph.textures.empty()) {
            return core::failure(std::string("declare a texture before adding a texture sample"));
        }
        node.name = state_.graph.textures.front().name;
    }
    State next = state_;
    next.layout[node.id] = position;
    const std::string id = node.id;
    next.graph.nodes.push_back(std::move(node));
    commit(std::move(next));
    return core::success(id);
}

EditResult GraphDocument::remove_nodes(const std::vector<std::string>& ids) {
    for (const std::string& id : ids) {
        const Node* node = find(id);
        if (node == nullptr) {
            return refuse("no node '" + id + "'");
        }
        if (node->kind == NodeKind::PbrOutput) {
            return refuse("the output node cannot be removed");
        }
    }
    State next = state_;
    for (const std::string& id : ids) {
        next.layout.erase(id);
    }
    next.graph.nodes.erase(std::remove_if(next.graph.nodes.begin(), next.graph.nodes.end(),
                                          [&ids](const Node& node) {
                                              return std::find(ids.begin(), ids.end(), node.id) != ids.end();
                                          }),
                           next.graph.nodes.end());
    // A link to a removed node goes with it, so the remaining graph names only nodes it has.
    for (Node& node : next.graph.nodes) {
        for (auto input = node.inputs.begin(); input != node.inputs.end();) {
            input = std::find(ids.begin(), ids.end(), input->second) == ids.end() ? std::next(input) : node.inputs.erase(input);
        }
    }
    commit(std::move(next));
    return core::success(core::Unit{});
}

EditResult GraphDocument::link(const std::string& consumer, const std::string& pin, const std::string& source) {
    const Node* target = find(consumer);
    if (target == nullptr) {
        return refuse("no node '" + consumer + "'");
    }
    if (find(source) == nullptr) {
        return refuse("no node '" + source + "'");
    }
    if (consumer == source) {
        return refuse("a node cannot feed itself");
    }
    if (!has_pin(target->kind, pin)) {
        return refuse(std::string(to_string(target->kind)) + " has no input '" + pin + "'");
    }
    State next = state_;
    next.graph.nodes[index_of(consumer)].inputs[pin] = source;
    commit(std::move(next));
    return core::success(core::Unit{});
}

EditResult GraphDocument::unlink(const std::string& consumer, const std::string& pin) {
    const Node* target = find(consumer);
    if (target == nullptr) {
        return refuse("no node '" + consumer + "'");
    }
    if (!target->inputs.contains(pin)) {
        return core::success(core::Unit{});
    }
    State next = state_;
    next.graph.nodes[index_of(consumer)].inputs.erase(pin);
    commit(std::move(next));
    return core::success(core::Unit{});
}

template <typename Change>
EditResult GraphDocument::change_node(const std::string& id, std::initializer_list<NodeKind> kinds, const char* what, Change change) {
    const std::size_t index = index_of(id);
    if (index == kNoNode) {
        return refuse("no node '" + id + "'");
    }
    if (std::find(kinds.begin(), kinds.end(), state_.graph.nodes[index].kind) == kinds.end()) {
        return refuse(std::string(to_string(state_.graph.nodes[index].kind)) + " has no " + what);
    }
    State next = state_;
    change(next.graph.nodes[index]);
    commit(std::move(next));
    return core::success(core::Unit{});
}

EditResult GraphDocument::set_value(const std::string& id, glm::vec4 value) {
    return change_node(id, {NodeKind::Constant, NodeKind::Parameter}, "value", [value](Node& node) { node.value = value; });
}

EditResult GraphDocument::set_type(const std::string& id, ValueType type) {
    return change_node(id, {NodeKind::Constant, NodeKind::Parameter}, "type", [type](Node& node) { node.type = type; });
}

EditResult GraphDocument::set_name(const std::string& id, const std::string& name) {
    return change_node(id, {NodeKind::Parameter, NodeKind::TextureSample}, "name", [&name](Node& node) { node.name = name; });
}

EditResult GraphDocument::set_op(const std::string& id, ArithmeticOp op) {
    return change_node(id, {NodeKind::Arithmetic}, "operation", [op](Node& node) { node.op = op; });
}

EditResult GraphDocument::add_texture(TextureDecl texture) {
    State next = state_;
    next.graph.textures.push_back(std::move(texture));
    commit(std::move(next));
    return core::success(core::Unit{});
}

EditResult GraphDocument::move_nodes(const std::vector<std::string>& ids, glm::vec2 offset) {
    for (const std::string& id : ids) {
        if (find(id) == nullptr) {
            return refuse("no node '" + id + "'");
        }
    }
    if (offset == glm::vec2(0.0f)) {
        return core::success(core::Unit{});
    }
    State next = state_;
    for (const std::string& id : ids) {
        const auto found = next.layout.find(id);
        const glm::vec2 current = found == next.layout.end() ? glm::vec2(0.0f) : found->second;
        next.layout[id] = current + offset;
    }
    commit(std::move(next));
    return core::success(core::Unit{});
}

bool GraphDocument::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(std::move(state_));
    state_ = std::move(undo_.back());
    undo_.pop_back();
    ++revision_;
    revalidate();
    return true;
}

bool GraphDocument::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(std::move(state_));
    state_ = std::move(redo_.back());
    redo_.pop_back();
    ++revision_;
    revalidate();
    return true;
}

void GraphDocument::select(const std::string& id, bool additive) {
    if (find(id) == nullptr) {
        return;
    }
    if (!additive) {
        selection_.clear();
    }
    if (additive && selection_.contains(id)) {
        selection_.erase(id);
        return;
    }
    selection_.insert(id);
}

core::Result<std::string, std::vector<GraphError>> GraphDocument::save_graph() const {
    if (!errors_.empty()) {
        return core::failure(errors_);
    }
    return core::success(graph_to_json(state_.graph));
}

std::string GraphDocument::save_layout() const {
    return layout_json(state_.layout);
}

core::Result<PreviewSwatch, std::vector<GraphError>> GraphDocument::preview(std::size_t size, const ParameterValues& overrides) const {
    core::Result<SurfaceEvaluator, std::vector<GraphError>> evaluator = SurfaceEvaluator::create(last_valid_, overrides);
    if (!evaluator.ok()) {
        return core::failure(evaluator.error());
    }
    const SurfaceEvaluator& surface = evaluator.value();

    PreviewSwatch swatch;
    swatch.size = size;
    swatch.base_color.reserve(size * size);
    swatch.roughness.reserve(size * size);
    for (std::size_t row = 0; row < size; ++row) {
        for (std::size_t column = 0; column < size; ++column) {
            const glm::vec2 uv((static_cast<float>(column) + 0.5f) / static_cast<float>(size),
                               (static_cast<float>(row) + 0.5f) / static_cast<float>(size));
            const MaterialSurface sample = surface.evaluate(uv);
            swatch.base_color.push_back(sample.base_color);
            swatch.roughness.push_back(sample.roughness);
        }
    }
    return core::success(std::move(swatch));
}

}  // namespace engine::material_graph
