// CPU checks of the editable material graph (US-068): edits and their refusals, undo and redo, the last valid version, the layout kept
// apart from the graph, selection, save and open round trips, and the preview of the working parameters.

#include "material_graph_fixture.hpp"

#include "engine/material_graph/codegen.hpp"
#include "engine/material_graph/edit.hpp"
#include "engine/material_graph/graph.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>

using engine::material_graph::ArithmeticOp;
using engine::material_graph::generate_hlsl;
using engine::material_graph::GraphDocument;
using engine::material_graph::graph_to_json;
using engine::material_graph::NodeKind;
using engine::material_graph::ParameterValues;
using engine::material_graph::ValueType;

namespace {

std::string graph_text(const GraphDocument& document) {
    return graph_to_json(document.graph());
}

// The sample graph opened with no layout, so the nodes sit on the grid.
GraphDocument open_sample() {
    auto opened = GraphDocument::open(engine::material_graph::tests::sample_graph_json(), "");
    REQUIRE(opened.ok());
    return std::move(opened).value();
}

}  // namespace

TEST_CASE("A new document holds one valid output and its last valid version is that graph", "[unit][material_graph]") {
    GraphDocument document = GraphDocument::create("fresh");
    CHECK(document.valid());
    CHECK(document.graph().nodes.size() == 2);
    CHECK(graph_text(document) == graph_to_json(document.last_valid()));
    CHECK(!document.can_undo());
    CHECK(!document.can_redo());
}

TEST_CASE("Adding and linking a node is undone and redone to the same graph", "[unit][material_graph]") {
    GraphDocument document = GraphDocument::create("undo");
    const std::string before = graph_text(document);

    auto added = document.add_node(NodeKind::Arithmetic, glm::vec2(100.0f, 40.0f));
    REQUIRE(added.ok());
    const std::string id = added.value();
    REQUIRE(document.link(id, "a", "base_color").ok());
    REQUIRE(document.link(id, "b", "base_color").ok());
    REQUIRE(document.link("output", "base_color", id).ok());
    const std::string after = graph_text(document);
    CHECK(after != before);

    // Four commits: the add, two links, and the output link. Four undos reach the starting graph, and four redos reach the end.
    for (int i = 0; i < 4; ++i) {
        CHECK(document.undo());
    }
    CHECK(graph_text(document) == before);
    CHECK(!document.can_undo());

    for (int i = 0; i < 4; ++i) {
        CHECK(document.redo());
    }
    CHECK(graph_text(document) == after);
    CHECK(!document.can_redo());
}

TEST_CASE("A refused edit names what it cannot do and leaves the document and its history unchanged", "[unit][material_graph]") {
    GraphDocument document = GraphDocument::create("refused");
    const std::string before = graph_text(document);

    CHECK(!document.link("missing", "a", "base_color").ok());
    CHECK(!document.link("output", "base_color", "output").ok());
    CHECK(!document.link("output", "no_such_pin", "base_color").ok());
    CHECK(!document.remove_nodes({"output"}).ok());
    CHECK(!document.set_value("output", glm::vec4(0.0f)).ok());
    CHECK(!document.add_node(NodeKind::PbrOutput, glm::vec2(0.0f)).ok());
    CHECK(!document.add_node(NodeKind::TextureSample, glm::vec2(0.0f)).ok());

    const auto refused = document.set_op("output", ArithmeticOp::Add);
    REQUIRE(!refused.ok());
    CHECK(refused.error().find("pbr_output") != std::string::npos);

    CHECK(graph_text(document) == before);
    CHECK(!document.can_undo());
}

TEST_CASE("A broken edit is kept for repair, its nodes are named, and the last valid version survives it", "[unit][material_graph]") {
    GraphDocument document = GraphDocument::create("broken");
    const std::string valid_text = graph_text(document);

    // A float output fed a vec4: a type error on the output node's roughness pin, with the graph otherwise intact.
    REQUIRE(document.link("output", "roughness", "base_color").ok());
    CHECK(!document.valid());
    CHECK(document.node_has_error("output"));
    CHECK(!document.node_has_error("base_color"));
    CHECK(graph_text(document) != valid_text);

    // The last valid version is still the graph before the break, so the preview and the save keep working from it.
    CHECK(graph_to_json(document.last_valid()) == valid_text);
    CHECK(document.preview(2).ok());
    CHECK(!document.save_graph().ok());

    // Undo returns to the valid graph, and the save works again.
    CHECK(document.undo());
    CHECK(document.valid());
    CHECK(document.save_graph().ok());
}

TEST_CASE("A cycle is an error on the nodes that form it", "[unit][material_graph]") {
    GraphDocument document = GraphDocument::create("cycle");
    const std::string first = document.add_node(NodeKind::Arithmetic, glm::vec2(0.0f)).value();
    const std::string second = document.add_node(NodeKind::Arithmetic, glm::vec2(0.0f)).value();
    REQUIRE(document.link(first, "a", second).ok());
    REQUIRE(document.link(second, "a", first).ok());

    CHECK(!document.valid());
    CHECK(document.node_has_error(first));
    CHECK(document.node_has_error(second));
}

TEST_CASE("Removing a node takes its links with it, and undo restores them", "[unit][material_graph]") {
    GraphDocument document = open_sample();
    const std::string before = graph_text(document);

    REQUIRE(document.remove_nodes({"tinted"}).ok());
    // The output's base colour pin is optional, so the graph stays valid, and the link to the removed node is gone.
    CHECK(document.valid());
    for (const auto& node : document.graph().nodes) {
        for (const auto& [pin, source] : node.inputs) {
            CHECK(source != "tinted");
        }
    }
    CHECK(document.graph().nodes.size() == 8);

    CHECK(document.undo());
    CHECK(graph_text(document) == before);
}

TEST_CASE("Moving nodes changes the layout and the undo history, not the graph or the generated source", "[unit][material_graph]") {
    GraphDocument document = open_sample();
    const std::string graph_before = graph_text(document);
    const std::string source_before = generate_hlsl(document.graph()).value().hash;

    REQUIRE(document.move_nodes({"tint", "rough"}, glm::vec2(30.0f, -12.0f)).ok());
    CHECK(graph_text(document) == graph_before);
    CHECK(generate_hlsl(document.graph()).value().hash == source_before);
    const glm::vec2 original = open_sample().layout().at("tint");
    CHECK(document.layout().at("tint") == original + glm::vec2(30.0f, -12.0f));

    CHECK(document.undo());
    CHECK(document.layout().at("tint") == original);
}

TEST_CASE("Selection holds node ids, toggles with the additive key, and drops a node that is removed", "[unit][material_graph]") {
    GraphDocument document = open_sample();

    document.select("tint", false);
    document.select("rough", true);
    CHECK(document.selection().size() == 2);
    document.select("rough", true);
    CHECK(document.selection().size() == 1);
    document.select("nope", false);
    CHECK(document.selection().size() == 1);

    REQUIRE(document.remove_nodes({"tint"}).ok());
    CHECK(document.selection().empty());
    // Selection is not history: the only entry is the removal.
    CHECK(document.can_undo());
    CHECK(document.undo());
    CHECK(!document.can_undo());
}

TEST_CASE("A saved graph opens to the same graph and layout, and a layout entry for a missing node is dropped", "[unit][material_graph]") {
    GraphDocument document = open_sample();
    REQUIRE(document.move_nodes({"out"}, glm::vec2(200.0f, 80.0f)).ok());

    const auto saved = document.save_graph();
    REQUIRE(saved.ok());
    const std::string layout = document.save_layout();

    auto reopened = GraphDocument::open(saved.value(), layout);
    REQUIRE(reopened.ok());
    CHECK(graph_text(reopened.value()) == graph_text(document));
    CHECK(reopened.value().save_layout() == layout);

    // A layout for a node that is not in the graph is ignored, and a node without a stored position is placed on the grid.
    const std::string stale = R"({"schema": "engine.material_graph.layout", "version": 1, "nodes": {"ghost": [1, 2]}})";
    auto without = GraphDocument::open(saved.value(), stale);
    REQUIRE(without.ok());
    CHECK(without.value().layout().count("ghost") == 0);
    CHECK(without.value().layout().count("out") == 1);
}

TEST_CASE("A layout document with a bad schema or position is refused by name", "[unit][material_graph]") {
    const std::string graph = engine::material_graph::tests::sample_graph_json();
    auto wrong_schema = GraphDocument::open(graph, R"({"schema": "engine.material_graph", "version": 1, "nodes": {}})");
    REQUIRE(!wrong_schema.ok());
    CHECK(wrong_schema.error().front().message.find("layout schema") != std::string::npos);

    auto bad_position = GraphDocument::open(graph, R"({"schema": "engine.material_graph.layout", "version": 1, "nodes": {"uv": [1]}})");
    REQUIRE(!bad_position.ok());
    CHECK(bad_position.error().front().message.find("'uv'") != std::string::npos);
}

TEST_CASE("The preview samples the working parameters, and an override changes it for that preview only", "[unit][material_graph]") {
    GraphDocument document = open_sample();

    auto defaults = document.preview(2);
    REQUIRE(defaults.ok());
    REQUIRE(defaults.value().base_color.size() == 4);
    // Unbound albedo is white, so the base colour is the tint's default: white.
    for (const glm::vec4& color : defaults.value().base_color) {
        CHECK(color == glm::vec4(1.0f));
    }

    ParameterValues overrides;
    overrides["tint"] = {0.25f, 0.5f, 1.0f, 1.0f};
    auto tinted = document.preview(2, overrides);
    REQUIRE(tinted.ok());
    CHECK(tinted.value().base_color.front() == glm::vec4(0.25f, 0.5f, 1.0f, 1.0f));

    // The override is for this preview only, so the next default preview is white again.
    CHECK(document.preview(2).value().base_color.front() == glm::vec4(1.0f));
}

TEST_CASE("A graph that breaks a rule is refused on open, and the error names its node", "[unit][material_graph]") {
    // The operands of the arithmetic node differ in type. Opening checks every rule, so the document never holds a state with errors
    // that was not made by an edit.
    auto refused = GraphDocument::open(R"({"schema": "engine.material_graph", "version": 1, "name": "x", "nodes": [
  {"id": "out", "kind": "pbr_output", "inputs": {"base_color": "bad"}},
  {"id": "c", "kind": "constant", "type": "vec4", "value": [1, 1, 1, 1]},
  {"id": "v", "kind": "constant", "type": "vec3", "value": [1, 1, 1]},
  {"id": "bad", "kind": "arithmetic", "op": "add", "inputs": {"a": "c", "b": "v"}}]})", "");
    REQUIRE(!refused.ok());
    CHECK(refused.error().front().node == "bad");
}

TEST_CASE("A texture sample needs a declared texture, and a declared one is used by default", "[unit][material_graph]") {
    GraphDocument document = GraphDocument::create("tex");
    CHECK(!document.add_node(NodeKind::TextureSample, glm::vec2(0.0f)).ok());

    engine::material_graph::TextureDecl texture;
    texture.name = "albedo";
    REQUIRE(document.add_texture(texture).ok());
    auto added = document.add_node(NodeKind::TextureSample, glm::vec2(0.0f));
    REQUIRE(added.ok());
    bool found = false;
    for (const auto& node : document.graph().nodes) {
        found = found || (node.id == added.value() && node.kind == NodeKind::TextureSample && node.name == "albedo");
    }
    CHECK(found);
}
