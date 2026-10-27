// CPU checks of the material graph (US-067): the versioned format, node-specific validation, the parameter block, the generated source's
// identity, and the CPU reference of the surface. The compile tests (material_graph_compile_tests.cpp) run the same graphs through DXC.

#include "material_graph_fixture.hpp"

#include "engine/material_graph/codegen.hpp"
#include "engine/material_graph/compile.hpp"
#include "engine/material_graph/evaluate.hpp"
#include "engine/material_graph/graph.hpp"
#include "engine/render/pbr.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

using engine::material_graph::Analysis;
using engine::material_graph::analyze;
using engine::material_graph::Graph;
using engine::material_graph::graph_to_json;
using engine::material_graph::MaterialVariant;
using engine::material_graph::material_variants;
using engine::material_graph::ParameterLayout;
using engine::material_graph::ParameterValues;
using engine::material_graph::pack_parameters;
using engine::material_graph::parameter_layout;
using engine::material_graph::parse_graph;
using engine::material_graph::SurfaceEvaluator;
using engine::material_graph::tests::edit;
using engine::material_graph::tests::errors_of;
using engine::material_graph::tests::parse_valid;
using engine::material_graph::tests::sample_graph_json;
using engine::material_graph::to_text;
using engine::material_graph::validate_graph;
using engine::render::ImageRgba8;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

// A document with one extra node list entry, appended before the closing bracket of "nodes".
std::string with_nodes(const std::string& extra) {
    return edit(sample_graph_json(), "{\"id\": \"out\"", extra + ",\n    {\"id\": \"out\"");
}

ImageRgba8 one_texel(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    ImageRgba8 image;
    image.width = 1;
    image.height = 1;
    image.pixels = {r, g, b, a};
    return image;
}

}  // namespace

TEST_CASE("A valid graph parses and its canonical JSON parses back to the same graph", "[unit][material_graph]") {
    const Graph graph = parse_valid(sample_graph_json());
    REQUIRE(graph.nodes.size() == 9);
    REQUIRE(graph.textures.size() == 2);

    const std::string canonical = graph_to_json(graph);
    const Graph again = parse_valid(canonical);
    REQUIRE(graph_to_json(again) == canonical);

    const Analysis analysis = analyze(graph);
    REQUIRE(analysis.errors.empty());
    REQUIRE(graph.nodes[analysis.output].id == "out");
}

TEST_CASE("A graph of another format version is refused with the version it names", "[unit][material_graph]") {
    const std::string message = errors_of(edit(sample_graph_json(), "\"version\": 1", "\"version\": 2"));
    REQUIRE_THAT(message, ContainsSubstring("unsupported graph version 2"));
    REQUIRE_THAT(errors_of(edit(sample_graph_json(), "\"schema\": \"engine.material_graph\"", "\"schema\": \"other\"")),
                 ContainsSubstring("schema must be"));
}

TEST_CASE("A field a node kind does not use is refused by name", "[unit][material_graph]") {
    const std::string message = errors_of(edit(sample_graph_json(), "{\"id\": \"uv\", \"kind\": \"uv\"}",
                                               "{\"id\": \"uv\", \"kind\": \"uv\", \"name\": \"oops\"}"));
    REQUIRE_THAT(message, ContainsSubstring("node 'uv' (uv): field 'name' does not apply"));
}

TEST_CASE("An arithmetic node with mismatched vector inputs names the node and both types", "[unit][material_graph]") {
    const std::string text = with_nodes(
        R"({"id": "c2", "kind": "parameter", "type": "vec2", "name": "offset", "value": [0, 0]},
    {"id": "mismatch", "kind": "arithmetic", "op": "add", "inputs": {"a": "tint", "b": "c2"}},
    {"id": "sink", "kind": "arithmetic", "op": "add", "inputs": {"a": "mismatch", "b": "rough"}})");
    const std::string message = errors_of(text);
    REQUIRE_THAT(message, ContainsSubstring("node 'mismatch' (arithmetic): inputs 'a' (vec4) and 'b' (vec2) must match"));
}

TEST_CASE("A pin fed by the wrong type is named with the type it expects", "[unit][material_graph]") {
    const std::string text = with_nodes(
        R"({"id": "c3", "kind": "parameter", "type": "vec3", "name": "offset3", "value": [0, 0, 0]},
    {"id": "tex_bad", "kind": "texture_sample", "name": "albedo", "inputs": {"uv": "c3"}})");
    REQUIRE_THAT(errors_of(text), ContainsSubstring("node 'tex_bad' (texture_sample): input 'uv' expects vec2, got vec3"));
}

TEST_CASE("A cycle is reported at the node whose input closes it", "[unit][material_graph]") {
    const std::string text = with_nodes(
        R"({"id": "loop_a", "kind": "arithmetic", "op": "add", "inputs": {"a": "loop_b", "b": "rough"}},
    {"id": "loop_b", "kind": "arithmetic", "op": "add", "inputs": {"a": "loop_a", "b": "rough"}})");
    const std::string message = errors_of(text);
    REQUIRE_THAT(message, ContainsSubstring("(arithmetic): input '"));
    REQUIRE_THAT(message, ContainsSubstring("closes a cycle through node"));
}

TEST_CASE("Unknown, unconnected, and dangling pins are each reported on their node", "[unit][material_graph]") {
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "bad_pin", "kind": "arithmetic", "op": "add", "inputs": {"a": "rough", "c": "rough"}})")),
                 ContainsSubstring("node 'bad_pin' (arithmetic): unknown input 'c'"));
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "half", "kind": "arithmetic", "op": "add", "inputs": {"a": "rough"}})")),
                 ContainsSubstring("node 'half' (arithmetic): input 'b' is not connected"));
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "dangling", "kind": "arithmetic", "op": "add", "inputs": {"a": "nope", "b": "rough"}})")),
                 ContainsSubstring("node 'dangling' (arithmetic): input 'a' names unknown node 'nope'"));
}

TEST_CASE("A graph needs exactly one PBR output", "[unit][material_graph]") {
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "second", "kind": "pbr_output"})")),
                 ContainsSubstring("the graph needs exactly one pbr_output node, found 2"));
    REQUIRE_THAT(errors_of(edit(sample_graph_json(), R"({"id": "out", "kind": "pbr_output", "inputs": {"base_color": "tinted", "roughness": "rough", "normal": "normal"}})",
                                R"({"id": "out", "kind": "uv"})")),
                 ContainsSubstring("found 0"));
}

TEST_CASE("Parameter names are unique and the alpha cutoff name is reserved", "[unit][material_graph]") {
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "dup", "kind": "parameter", "type": "float", "name": "roughness", "value": [0.5]})")),
                 ContainsSubstring("parameter 'roughness' is already declared by node 'rough'"));
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "cut", "kind": "parameter", "type": "float", "name": "alpha_cutoff", "value": [0.5]})")),
                 ContainsSubstring("reserved for every material's alpha cutoff"));
}

TEST_CASE("A texture sample of an undeclared texture is refused", "[unit][material_graph]") {
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "ghost", "kind": "texture_sample", "name": "missing", "inputs": {"uv": "uv"}})")),
                 ContainsSubstring("node 'ghost' (texture_sample): texture 'missing' is not declared"));
}

TEST_CASE("Constants and parameters need a valid type and a value of that many finite numbers", "[unit][material_graph]") {
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "c", "kind": "constant", "type": "vec3", "value": [1, 2]})")),
                 ContainsSubstring("value must be an array of 3 finite numbers"));
    REQUIRE_THAT(errors_of(with_nodes(R"({"id": "c", "kind": "constant", "type": "quat", "value": [1]})")),
                 ContainsSubstring("type must be float, vec2, vec3, or vec4"));
}

TEST_CASE("The parameter block reserves row 0 for the alpha cutoff and orders the rest by name", "[unit][material_graph]") {
    const Graph graph = parse_valid(sample_graph_json());
    const ParameterLayout layout = parameter_layout(graph);
    REQUIRE(layout.entries.size() == 4);
    REQUIRE(layout.entries[0].name == "alpha_cutoff");
    REQUIRE(layout.entries[1].name == "normal_scale");
    REQUIRE(layout.entries[2].name == "roughness");
    REQUIRE(layout.entries[3].name == "tint");
    for (std::uint32_t i = 0; i < 4; ++i) {
        REQUIRE(layout.entries[i].row == i);
    }
    REQUIRE(layout.size_bytes() == 64);
    REQUIRE(layout.entries[0].default_value.x == 0.5f);
}

TEST_CASE("Packing writes each instance value into its row and rejects values that do not fit", "[unit][material_graph]") {
    const Graph graph = parse_valid(sample_graph_json());
    const ParameterLayout layout = parameter_layout(graph);

    ParameterValues values;
    values["tint"] = {0.5f, 0.25f, 0.125f, 1.0f};
    values["alpha_cutoff"] = {0.75f};
    auto packed = pack_parameters(layout, values);
    REQUIRE(packed.ok());
    REQUIRE(packed.value().size() == 64);
    float floats[16];
    std::memcpy(floats, packed.value().data(), sizeof floats);
    REQUIRE(floats[0] == 0.75f);            // alpha_cutoff, row 0
    REQUIRE(floats[4] == 1.0f);             // normal_scale, default
    REQUIRE(floats[8] == 0.25f);            // roughness, default
    REQUIRE(floats[12] == 0.5f);            // tint, row 3
    REQUIRE(floats[13] == 0.25f);
    REQUIRE(floats[14] == 0.125f);
    REQUIRE(floats[15] == 1.0f);

    ParameterValues wrong_count;
    wrong_count["tint"] = {1.0f, 1.0f, 1.0f};
    auto rejected = pack_parameters(layout, wrong_count);
    REQUIRE_FALSE(rejected.ok());
    REQUIRE_THAT(to_text(rejected.error().front()), ContainsSubstring("parameter 'tint' expects 4 values, got 3"));

    ParameterValues unknown;
    unknown["nope"] = {1.0f};
    REQUIRE_THAT(to_text(pack_parameters(layout, unknown).error().front()), ContainsSubstring("is not a parameter of this material"));

    ParameterValues not_finite;
    not_finite["roughness"] = {std::numeric_limits<float>::quiet_NaN()};
    REQUIRE_THAT(to_text(pack_parameters(layout, not_finite).error().front()), ContainsSubstring("is not finite"));
}

TEST_CASE("Changing a parameter value leaves the generated source as it is; changing a constant or the topology does not", "[unit][material_graph]") {
    const auto hash_of = [](const std::string& text) {
        auto generated = engine::material_graph::generate_hlsl(parse_valid(text));
        REQUIRE(generated.ok());
        return generated.value().hash;
    };
    const std::string base = hash_of(sample_graph_json());

    // A parameter default changes the block's defaults, not the code. An instance overrides values at pack time, not in the graph.
    CHECK(hash_of(edit(sample_graph_json(), "\"value\": [1, 1, 1, 1]", "\"value\": [0.5, 0.5, 0.5, 1]")) == base);

    // A constant is baked into the source, and so is a new node.
    const std::string with_constant = with_nodes(R"({"id": "k", "kind": "constant", "type": "float", "value": [0.5]})");
    CHECK(hash_of(with_constant) != base);
    CHECK(hash_of(edit(with_constant, "\"value\": [0.5]", "\"value\": [0.75]")) != hash_of(with_constant));

    // A renamed parameter changes the block and the code that reads it.
    CHECK(hash_of(edit(sample_graph_json(), "\"name\": \"tint\"", "\"name\": \"colour\"")) != base);
}

TEST_CASE("The generated source reads its parameters, textures, and variants the way the GPU pass expects", "[unit][material_graph]") {
    const auto generated = engine::material_graph::generate_hlsl(parse_valid(sample_graph_json()));
    REQUIRE(generated.ok());
    const std::string& hlsl = generated.value().hlsl;
    REQUIRE_THAT(hlsl, ContainsSubstring("cbuffer MgParams : register(b0, space1)"));
    REQUIRE_THAT(hlsl, ContainsSubstring("Texture2D<float4> t_albedo : register(t1, space1);"));
    REQUIRE_THAT(hlsl, ContainsSubstring("Texture2D<float4> t_normal : register(t2, space1);"));
    REQUIRE_THAT(hlsl, ContainsSubstring("n_albedo_tex.rgb = mg_srgb_to_linear(n_albedo_tex.rgb);"));
    REQUIRE_THAT(hlsl, ContainsSubstring("float4 n_tint = p_tint;"));
    REQUIRE_THAT(hlsl, ContainsSubstring("#if MG_BLEND_MODE == 1"));
    REQUIRE_THAT(hlsl, ContainsSubstring("PSGraph"));
    REQUIRE(generated.value().textures.size() == 2);
    REQUIRE(generated.value().textures[0].name == "albedo");
}

TEST_CASE("The material variants are the three blend modes, each with and without two-sided rendering", "[unit][material_graph]") {
    const std::vector<MaterialVariant> variants = material_variants();
    REQUIRE(variants.size() == 6);
    std::set<std::string> names;
    for (const MaterialVariant& variant : variants) {
        names.insert(variant.name());
    }
    REQUIRE(names.size() == 6);
    REQUIRE(names.contains("opaque"));
    REQUIRE(names.contains("alpha_mask_two_sided"));
    REQUIRE(names.contains("alpha_blend"));
}

TEST_CASE("The CPU surface follows the graph: parameters, the sRGB decode, and the PBR clamps", "[unit][material_graph]") {
    const Graph graph = parse_valid(sample_graph_json());
    ParameterValues instance;
    instance["tint"] = {0.5f, 0.25f, 0.125f, 1.0f};
    instance["roughness"] = {0.75f};

    auto created = SurfaceEvaluator::create(graph, instance);
    REQUIRE(created.ok());
    SurfaceEvaluator evaluator = std::move(created).value();

    // Unbound textures sample white, so the base color is the tint alone.
    const auto unbound = evaluator.evaluate({0.3f, 0.6f});
    REQUIRE_THAT(unbound.base_color.r, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(unbound.base_color.g, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(unbound.roughness, WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(unbound.normal.z, WithinAbs(1.0f, 1e-3f));
    REQUIRE_THAT(unbound.normal.x, WithinAbs(0.0f, 1e-2f));

    // A bound sRGB texture is decoded before the tint. Two texels, repeat addressing: u = 0.75 reads the second texel, u = 1.25 the first.
    ImageRgba8 albedo;
    albedo.width = 2;
    albedo.height = 1;
    albedo.pixels = {0, 0, 0, 255, 128, 64, 255, 255};
    REQUIRE_FALSE(evaluator.bind_texture("albedo", &albedo).has_value());
    const auto second = evaluator.evaluate({0.75f, 0.5f});
    REQUIRE_THAT(second.base_color.r, WithinAbs(engine::render::srgb_to_linear(128.0f / 255.0f) * 0.5f, 1e-5f));
    REQUIRE_THAT(second.base_color.b, WithinAbs(engine::render::srgb_to_linear(1.0f) * 0.125f, 1e-5f));
    const auto wrapped = evaluator.evaluate({1.25f, 0.5f});
    REQUIRE_THAT(wrapped.base_color.r, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(wrapped.base_color.g, WithinAbs(0.0f, 1e-6f));

    // A tangent-space normal decodes from rgb * 2 - 1; a zero scale flattens it to the surface normal.
    ImageRgba8 normal = one_texel(255, 128, 128, 255);
    REQUIRE_FALSE(evaluator.bind_texture("normal", &normal).has_value());
    const auto bent = evaluator.evaluate({0.0f, 0.0f});
    REQUIRE(bent.normal.x > 0.99f);
    REQUIRE(bent.normal.z < 0.01f);
    REQUIRE_THAT(glm::length(bent.normal), WithinAbs(1.0f, 1e-5f));

    ParameterValues flat = instance;
    flat["normal_scale"] = {0.0f};
    auto flat_eval = SurfaceEvaluator::create(graph, flat);
    REQUIRE(flat_eval.ok());
    SurfaceEvaluator flat_evaluator = std::move(flat_eval).value();
    REQUIRE_FALSE(flat_evaluator.bind_texture("normal", &normal).has_value());
    const auto unbent = flat_evaluator.evaluate({0.0f, 0.0f});
    REQUIRE_THAT(unbent.normal.x, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(unbent.normal.z, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("Arithmetic broadcasts a float over a vector, and PBR pins clamp to their ranges", "[unit][material_graph]") {
    const std::string text = edit(
        sample_graph_json(),
        R"({"id": "out", "kind": "pbr_output", "inputs": {"base_color": "tinted", "roughness": "rough", "normal": "normal"}})",
        R"({"id": "half", "kind": "constant", "type": "float", "value": [0.5]},
    {"id": "palette", "kind": "constant", "type": "vec4", "value": [1, 0.5, 0.25, 1]},
    {"id": "scaled", "kind": "arithmetic", "op": "multiply", "inputs": {"a": "half", "b": "palette"}},
    {"id": "rgb", "kind": "constant", "type": "vec3", "value": [0.2, 0.4, 0.6]},
    {"id": "over", "kind": "constant", "type": "float", "value": [2.5]},
    {"id": "out", "kind": "pbr_output", "inputs": {"base_color": "scaled", "metallic": "over", "emissive": "rgb", "roughness": "rough"}})");
    const Graph graph = parse_valid(text);
    auto created = SurfaceEvaluator::create(graph, {});
    REQUIRE(created.ok());
    const auto surface = std::move(created).value().evaluate({0.0f, 0.0f});
    REQUIRE_THAT(surface.base_color.r, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(surface.base_color.g, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(surface.base_color.b, WithinAbs(0.125f, 1e-6f));
    REQUIRE_THAT(surface.alpha, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(surface.metallic, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(surface.emissive.b, WithinAbs(0.6f, 1e-6f));
}

TEST_CASE("A vec3 feeding a vec4 pin keeps its xyz and takes w = 1", "[unit][material_graph]") {
    const std::string text = edit(
        sample_graph_json(),
        R"({"id": "out", "kind": "pbr_output", "inputs": {"base_color": "tinted", "roughness": "rough", "normal": "normal"}})",
        R"({"id": "rgb", "kind": "constant", "type": "vec3", "value": [0.2, 0.4, 0.6]},
    {"id": "out", "kind": "pbr_output", "inputs": {"base_color": "rgb", "roughness": "rough", "normal": "normal"}})");
    auto created = SurfaceEvaluator::create(parse_valid(text), {});
    REQUIRE(created.ok());
    const auto surface = std::move(created).value().evaluate({0.0f, 0.0f});
    REQUIRE_THAT(surface.base_color.g, WithinAbs(0.4f, 1e-6f));
    REQUIRE_THAT(surface.base_color.a, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("The evaluator refuses an invalid instance and an undeclared texture", "[unit][material_graph]") {
    const Graph graph = parse_valid(sample_graph_json());
    ParameterValues bad;
    bad["tint"] = {1.0f};
    REQUIRE_FALSE(SurfaceEvaluator::create(graph, bad).ok());

    auto created = SurfaceEvaluator::create(graph, {});
    REQUIRE(created.ok());
    SurfaceEvaluator evaluator = std::move(created).value();
    REQUIRE(evaluator.bind_texture("ghost", nullptr).has_value());
}

TEST_CASE("analyze() lists every node in an order where each input comes first", "[unit][material_graph]") {
    const Graph graph = parse_valid(sample_graph_json());
    const Analysis analysis = analyze(graph);
    REQUIRE(analysis.order.size() == graph.nodes.size());
    std::vector<std::size_t> position(graph.nodes.size());
    for (std::size_t i = 0; i < analysis.order.size(); ++i) {
        position[analysis.order[i]] = i;
    }
    for (std::size_t u = 0; u < graph.nodes.size(); ++u) {
        for (const auto& [pin, source] : graph.nodes[u].inputs) {
            CHECK(position[analysis.index.at(source)] < position[u]);
        }
    }
    CHECK(validate_graph(graph).empty());
}
