#pragma once

// Material graph (US-067): a versioned DAG of material nodes, its JSON form, and its validator. The graph is the source of truth;
// codegen.hpp turns it into HLSL, evaluate.hpp runs it on the CPU, and compile.hpp builds it for both shader targets.
//
// Format (schema "engine.material_graph", version 1): {"schema", "version", "name", "textures": [{"name", "color_space"}], "nodes":
// [{"id", "kind", ...}]}. A node names its inputs by pin: "inputs": {"a": "<node id>"}. Kinds and their fields:
//   constant       type, value                 a literal, baked into the shader
//   parameter      type, name, value           an instance parameter; value is the default, changed per instance without a rebuild
//   uv             -                           the texture coordinate, vec2
//   texture_sample name (a declared texture), inputs.uv (vec2); the output is vec4, sRGB decoded when the texture says so.
//                  A texture is declared as {"name", "color_space": "srgb" | "linear", "fallback": "white" | "black" | "flat_normal"}.
//   arithmetic     op, inputs.a, inputs.b      add, subtract, multiply, divide, min, max; equal types, or one side a float
//   normal_map     inputs.texture (vec4), inputs.scale (float, default 1); the output is a tangent-space vec3
//   pbr_output     inputs: base_color (vec4), metallic, roughness, emissive (vec3), occlusion, alpha, normal (vec3); exactly one
//
// Pin types. A float feeds any vector pin (it is splatted), and a vec3 feeds a vec4 pin (w = 1). Nothing else converts.
// Errors are node-specific: each names the node, its kind, and the pin, so an editor can highlight it.

#include "engine/core/result.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace engine::material_graph {

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::string_view kSchemaName = "engine.material_graph";
// The alpha cutoff is a parameter of every material (MaterialParams::alpha_cutoff), so a graph may not declare its own.
inline constexpr std::string_view kAlphaCutoffParameter = "alpha_cutoff";

enum class ValueType : std::uint8_t {
    Float,
    Vec2,
    Vec3,
    Vec4,
};
[[nodiscard]] std::string_view to_string(ValueType type) noexcept;
// The number of components a type carries: 1 to 4.
[[nodiscard]] std::uint32_t components(ValueType type) noexcept;

enum class NodeKind : std::uint8_t {
    Constant,
    Parameter,
    Uv,
    TextureSample,
    Arithmetic,
    NormalMap,
    PbrOutput,
};
[[nodiscard]] std::string_view to_string(NodeKind kind) noexcept;

enum class ArithmeticOp : std::uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide,
    Min,
    Max,
};
[[nodiscard]] std::string_view to_string(ArithmeticOp op) noexcept;

enum class ColorSpace : std::uint8_t {
    Linear,
    Srgb,
};
[[nodiscard]] std::string_view to_string(ColorSpace space) noexcept;

// What an unbound texture samples as. It matches the renderer's default texture for an empty slot: white for color, a flat normal
// for normal maps.
enum class Fallback : std::uint8_t {
    White,
    Black,
    FlatNormal,
};
[[nodiscard]] std::string_view to_string(Fallback fallback) noexcept;

struct TextureDecl {
    std::string name;
    ColorSpace color_space = ColorSpace::Srgb;
    Fallback fallback = Fallback::White;
};

struct Node {
    std::string id;  // unique, and an identifier, because codegen names a local with it
    NodeKind kind = NodeKind::Constant;
    ValueType type = ValueType::Float;  // Constant and Parameter: the declared type
    glm::vec4 value{0.0f};              // Constant: the value. Parameter: the default. Only the first components(type) are meaningful
    std::string name;                   // Parameter: the parameter name. TextureSample: the texture name
    ArithmeticOp op = ArithmeticOp::Add;
    std::map<std::string, std::string> inputs;  // pin name -> id of the node that feeds it
};

struct Graph {
    std::uint32_t version = kFormatVersion;
    std::string name;
    std::vector<TextureDecl> textures;
    std::vector<Node> nodes;
};

struct GraphError {
    std::string node;  // the node id; empty for a graph-level problem
    std::string kind;  // the node's kind name; empty for a graph-level problem
    std::string message;
};
// "node 'mul' (arithmetic): input 'b' expects vec3, got vec2", or the bare message for a graph-level problem.
[[nodiscard]] std::string to_text(const GraphError& error);

// One pin of a node kind. A required pin must be linked; an optional pin falls back to `fallback` when it is not.
struct PinSpec {
    std::string name;
    ValueType type = ValueType::Float;
    bool required = false;
    bool any_type = false;  // arithmetic operands: any type, checked against each other by analyze()
    glm::vec4 fallback{0.0f};
};
[[nodiscard]] std::vector<PinSpec> pin_specs(NodeKind kind);
// Whether a value of `source` may feed a pin of `target` (exact, a float splat, or a vec3 with w = 1).
[[nodiscard]] bool accepts(ValueType source, ValueType target) noexcept;

// The result of checking a graph. When `errors` is empty, `order` lists every node with each input before its consumer, `types`
// holds each node's output type, `index` maps an id to its position in `nodes`, and `output` is the PBR output's position.
struct Analysis {
    std::vector<GraphError> errors;
    std::vector<std::size_t> order;
    std::vector<ValueType> types;
    std::map<std::string, std::size_t, std::less<>> index;
    std::size_t output = 0;
};
[[nodiscard]] Analysis analyze(const Graph& graph);

// Every problem analyze() finds, in node order. Empty when the graph is valid.
[[nodiscard]] std::vector<GraphError> validate_graph(const Graph& graph);

// Parses and validates. A document that is not JSON, has the wrong schema or version, or breaks a field's shape fails before the
// graph checks run, and each failure names the node or field.
[[nodiscard]] core::Result<Graph, std::vector<GraphError>> parse_graph(std::string_view json);

// The canonical document: object keys sorted, two-space indent, one trailing newline. parse_graph(graph_to_json(g)) == g.
[[nodiscard]] std::string graph_to_json(const Graph& graph);

}  // namespace engine::material_graph
