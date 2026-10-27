#include "engine/material_graph/codegen.hpp"

#include "engine/assets/content_hash.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace engine::material_graph {
namespace {

// The reserved row: alpha_cutoff is the first parameter of every material, so MaterialParams::alpha_cutoff has a home in the block.
constexpr float kDefaultAlphaCutoff = 0.5f;

std::string hlsl_type(ValueType type) {
    switch (type) {
        case ValueType::Float:
            return "float";
        case ValueType::Vec2:
            return "float2";
        case ValueType::Vec3:
            return "float3";
        case ValueType::Vec4:
            return "float4";
    }
    return "float";
}

// The HLSL swizzle that selects the first components(type) components of a float4 row.
std::string swizzle(ValueType type) {
    switch (type) {
        case ValueType::Float:
            return ".x";
        case ValueType::Vec2:
            return ".xy";
        case ValueType::Vec3:
            return ".xyz";
        case ValueType::Vec4:
            return "";
    }
    return "";
}

// A float literal that HLSL reads as a float (never as an int). %.9g round-trips a 32-bit float.
std::string float_literal(float value) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.9g", static_cast<double>(value));
    std::string text = buffer;
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string literal(const glm::vec4& value, ValueType type) {
    if (type == ValueType::Float) {
        return float_literal(value.x);
    }
    std::string text = hlsl_type(type) + "(";
    for (std::uint32_t i = 0; i < components(type); ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += float_literal(value[static_cast<int>(i)]);
    }
    return text + ")";
}

GraphError graph_error_for_parameter(const std::string& name, const std::string& problem) {
    return GraphError{"", "", "parameter '" + name + "' " + problem};
}

// Converts an expression between pin types. Only the conversions accepts() allows reach this.
std::string convert(const std::string& expr, ValueType from, ValueType to) {
    if (from == to) {
        return expr;
    }
    if (from == ValueType::Float) {
        return hlsl_type(to) + "(" + expr + ")";
    }
    if (from == ValueType::Vec3 && to == ValueType::Vec4) {
        return "float4(" + expr + ", 1.0)";
    }
    return expr;
}

// Prints a graph name inside a // comment: printable ASCII only, so no line can break out of it.
std::string comment_safe(const std::string& text) {
    std::string safe = text;
    for (char& c : safe) {
        if (c < 0x20 || c > 0x7e) {
            c = '?';
        }
    }
    return safe;
}

class Emitter {
public:
    Emitter(const Graph& graph, const Analysis& analysis) : graph_(graph), analysis_(analysis) {}

    // The expression that yields a node's input on a pin, converted to `target`. An unlinked pin yields its fallback.
    std::string input(const Node& node, const std::string& pin_name, ValueType target) const {
        const PinSpec* spec = nullptr;
        const std::vector<PinSpec> specs = pin_specs(node.kind);
        for (const PinSpec& candidate : specs) {
            if (candidate.name == pin_name) {
                spec = &candidate;
            }
        }
        const auto linked = node.inputs.find(pin_name);
        if (linked != node.inputs.end()) {
            const std::size_t source = analysis_.index.find(linked->second)->second;
            return convert("n_" + linked->second, analysis_.types[source], target);
        }
        return convert(literal(spec->fallback, spec->type), spec->type, target);
    }

    std::string statement(std::size_t u) const {
        const Node& node = graph_.nodes[u];
        const ValueType type = analysis_.types[u];
        const std::string name = "n_" + node.id;
        std::string text;
        switch (node.kind) {
            case NodeKind::Constant:
                text = "    " + hlsl_type(type) + " " + name + " = " + literal(node.value, type) + ";\n";
                break;
            case NodeKind::Parameter:
                text = "    " + hlsl_type(type) + " " + name + " = p_" + node.name + swizzle(type) + ";\n";
                break;
            case NodeKind::Uv:
                text = "    float2 " + name + " = uv;\n";
                break;
            case NodeKind::TextureSample:
                text = "    float4 " + name + " = mg_fetch(t_" + node.name + ", " + input(node, "uv", ValueType::Vec2) + ");\n";
                if (texture_is_srgb(node.name)) {
                    text += "    " + name + ".rgb = mg_srgb_to_linear(" + name + ".rgb);\n";
                }
                break;
            case NodeKind::Arithmetic: {
                const std::string a = input(node, "a", type);
                const std::string b = input(node, "b", type);
                std::string expr;
                switch (node.op) {
                    case ArithmeticOp::Add:
                        expr = "(" + a + ") + (" + b + ")";
                        break;
                    case ArithmeticOp::Subtract:
                        expr = "(" + a + ") - (" + b + ")";
                        break;
                    case ArithmeticOp::Multiply:
                        expr = "(" + a + ") * (" + b + ")";
                        break;
                    case ArithmeticOp::Divide:
                        expr = "(" + a + ") / (" + b + ")";
                        break;
                    case ArithmeticOp::Min:
                        expr = "min(" + a + ", " + b + ")";
                        break;
                    case ArithmeticOp::Max:
                        expr = "max(" + a + ", " + b + ")";
                        break;
                }
                text = "    " + hlsl_type(type) + " " + name + " = " + expr + ";\n";
                break;
            }
            case NodeKind::NormalMap: {
                // Tangent-space decode: n = normalize(xy * scale, 2z - 1) from the stored rgb * 2 - 1.
                const std::string src = "nm_src_" + node.id;
                const std::string dec = "nm_t_" + node.id;
                text = "    float4 " + src + " = " + input(node, "texture", ValueType::Vec4) + ";\n";
                text += "    float3 " + dec + " = " + src + ".rgb * 2.0 - 1.0;\n";
                text += "    float3 " + name + " = normalize(float3(" + dec + ".xy * " + input(node, "scale", ValueType::Float) + ", " +
                        dec + ".z));\n";
                break;
            }
            case NodeKind::PbrOutput:
                break;
        }
        return text;
    }

    std::string surface() const {
        const Node& node = graph_.nodes[analysis_.output];
        std::string text;
        text += "    MgSurface s;\n";
        text += "    s.base_color = saturate(" + input(node, "base_color", ValueType::Vec4) + ");\n";
        text += "    s.metallic = saturate(" + input(node, "metallic", ValueType::Float) + ");\n";
        text += "    s.roughness = saturate(" + input(node, "roughness", ValueType::Float) + ");\n";
        text += "    s.emissive = max(" + input(node, "emissive", ValueType::Vec3) + ", float3(0.0, 0.0, 0.0));\n";
        text += "    s.occlusion = saturate(" + input(node, "occlusion", ValueType::Float) + ");\n";
        text += "    s.normal = " + input(node, "normal", ValueType::Vec3) + ";\n";
        text += "    s.alpha = saturate(s.base_color.a * " + input(node, "alpha", ValueType::Float) + ");\n";
        text += "    return s;\n";
        return text;
    }

    bool texture_is_srgb(const std::string& name) const {
        for (const TextureDecl& texture : graph_.textures) {
            if (texture.name == name) {
                return texture.color_space == ColorSpace::Srgb;
            }
        }
        return false;
    }

private:
    const Graph& graph_;
    const Analysis& analysis_;
};

// The HLSL prelude: the macros the variants set, the material block, the surface and target structs, and the helpers. The helpers
// match the GPU reference in scene.hlsl (FetchTexture, SrgbToLinear) so a graph reads its textures the way the scene does.
constexpr std::string_view kPrelude = R"HLSL(
#ifndef MG_BLEND_MODE
#define MG_BLEND_MODE 0
#endif
#ifndef MG_TWO_SIDED
#define MG_TWO_SIDED 0
#endif

struct MgSurface
{
    float4 base_color;
    float metallic;
    float roughness;
    float3 emissive;
    float occlusion;
    float3 normal;
    float alpha;
};

struct MgTarget
{
    float4 color : SV_Target0;
    float4 normal_roughness : SV_Target1;
    float4 emissive_occlusion : SV_Target2;
};

float mg_srgb_channel(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 mg_srgb_to_linear(float3 c)
{
    return float3(mg_srgb_channel(c.x), mg_srgb_channel(c.y), mg_srgb_channel(c.z));
}

// Nearest texel with repeat addressing, as FetchTexture in scene.hlsl.
float4 mg_fetch(Texture2D<float4> map, float2 uv)
{
    uint width;
    uint height;
    map.GetDimensions(width, height);
    float2 f = frac(uv);
    uint2 texel = min(uint2(floor(f * float2(width, height))), uint2(width, height) - 1);
    return map.Load(int3(texel, 0));
}
)HLSL";

}  // namespace

const ParameterEntry* ParameterLayout::find(std::string_view name) const noexcept {
    for (const ParameterEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

ParameterLayout parameter_layout(const Graph& graph) {
    ParameterLayout layout;
    ParameterEntry reserved;
    reserved.name = std::string(kAlphaCutoffParameter);
    reserved.type = ValueType::Float;
    reserved.default_value = glm::vec4(kDefaultAlphaCutoff, 0.0f, 0.0f, 0.0f);
    layout.entries.push_back(reserved);

    std::vector<ParameterEntry> declared;
    for (const Node& node : graph.nodes) {
        if (node.kind == NodeKind::Parameter) {
            ParameterEntry entry;
            entry.name = node.name;
            entry.type = node.type;
            entry.default_value = node.value;
            declared.push_back(entry);
        }
    }
    std::sort(declared.begin(), declared.end(), [](const ParameterEntry& a, const ParameterEntry& b) { return a.name < b.name; });
    for (ParameterEntry& entry : declared) {
        layout.entries.push_back(std::move(entry));
    }
    for (std::size_t i = 0; i < layout.entries.size(); ++i) {
        layout.entries[i].row = static_cast<std::uint32_t>(i);
    }
    return layout;
}

core::Result<std::map<std::string, glm::vec4, std::less<>>, std::vector<GraphError>> resolve_parameters(
    const ParameterLayout& layout, const ParameterValues& values) {
    std::vector<GraphError> errors;
    for (const auto& [name, numbers] : values) {
        const ParameterEntry* entry = layout.find(name);
        if (entry == nullptr) {
            errors.push_back(graph_error_for_parameter(name, "is not a parameter of this material"));
            continue;
        }
        if (numbers.size() != components(entry->type)) {
            errors.push_back(graph_error_for_parameter(name, "expects " + std::to_string(components(entry->type)) + " values, got " +
                                                                 std::to_string(numbers.size())));
            continue;
        }
        for (const float number : numbers) {
            if (!std::isfinite(number)) {
                errors.push_back(graph_error_for_parameter(name, "has a value that is not finite"));
                break;
            }
        }
    }
    if (!errors.empty()) {
        return core::failure(std::move(errors));
    }

    std::map<std::string, glm::vec4, std::less<>> resolved;
    for (const ParameterEntry& entry : layout.entries) {
        glm::vec4 value = entry.default_value;
        const auto found = values.find(entry.name);
        if (found != values.end()) {
            value = glm::vec4(0.0f);
            for (std::size_t i = 0; i < found->second.size(); ++i) {
                value[static_cast<int>(i)] = found->second[i];
            }
        }
        resolved.emplace(entry.name, value);
    }
    return core::success(std::move(resolved));
}

core::Result<std::vector<std::uint8_t>, std::vector<GraphError>> pack_parameters(const ParameterLayout& layout,
                                                                                 const ParameterValues& values) {
    auto resolved = resolve_parameters(layout, values);
    if (!resolved.ok()) {
        return core::failure(std::move(resolved.error()));
    }
    std::vector<float> floats(static_cast<std::size_t>(layout.size_bytes() / sizeof(float)), 0.0f);
    for (const ParameterEntry& entry : layout.entries) {
        const glm::vec4& value = resolved.value().at(entry.name);
        for (std::uint32_t i = 0; i < components(entry.type); ++i) {
            floats[entry.row * 4 + i] = value[static_cast<int>(i)];
        }
    }
    std::vector<std::uint8_t> bytes(floats.size() * sizeof(float));
    std::memcpy(bytes.data(), floats.data(), bytes.size());
    return core::success(std::move(bytes));
}

std::vector<TextureDecl> texture_slots(const Graph& graph) {
    std::vector<TextureDecl> slots = graph.textures;
    std::sort(slots.begin(), slots.end(), [](const TextureDecl& a, const TextureDecl& b) { return a.name < b.name; });
    return slots;
}

core::Result<GeneratedShader, std::vector<GraphError>> generate_hlsl(const Graph& graph) {
    Analysis analysis = analyze(graph);
    if (!analysis.errors.empty()) {
        return core::failure(std::move(analysis.errors));
    }

    GeneratedShader shader;
    shader.parameters = parameter_layout(graph);
    shader.textures = texture_slots(graph);
    const Emitter emitter(graph, analysis);

    std::string hlsl = "// Generated by engine::material_graph (US-067) from graph '" + comment_safe(graph.name) +
                       "'. Do not edit: change the graph and rebuild.\n";
    hlsl += std::string(kPrelude);

    hlsl += "\ncbuffer " + std::string(kParameterBlockName) + " : register(b0, space" + std::to_string(kMaterialSet) + ")\n{\n";
    for (const ParameterEntry& entry : shader.parameters.entries) {
        hlsl += "    float4 p_" + entry.name + ";\n";
    }
    hlsl += "};\n";
    for (std::size_t i = 0; i < shader.textures.size(); ++i) {
        hlsl += "Texture2D<float4> t_" + shader.textures[i].name + " : register(t" + std::to_string(kFirstTextureBinding + i) + ", space" +
                std::to_string(kMaterialSet) + ");\n";
    }

    hlsl += "\nMgSurface MgEvaluate(float2 uv)\n{\n";
    for (const std::size_t u : analysis.order) {
        hlsl += emitter.statement(u);
    }
    hlsl += emitter.surface();
    hlsl += "}\n";

    hlsl += R"HLSL(
MgTarget PSGraph(float2 uv : TEXCOORD0, bool front : SV_IsFrontFace)
{
    MgSurface s = MgEvaluate(uv);
#if MG_TWO_SIDED
    if (!front)
    {
        s.normal = -s.normal;
    }
#endif
#if MG_BLEND_MODE == 1
    if (s.alpha < p_alpha_cutoff.x)
    {
        discard;
    }
#endif
    MgTarget o;
    o.color = float4(s.base_color.rgb, s.alpha);
    o.normal_roughness = float4(s.normal * 0.5 + 0.5, s.roughness);
    o.emissive_occlusion = float4(s.emissive, s.occlusion);
    return o;
}
)HLSL";

    shader.hash = assets::ContentHash::of(hlsl).to_hex();
    shader.hlsl = std::move(hlsl);
    return core::success(std::move(shader));
}

}  // namespace engine::material_graph
