#pragma once

// HLSL generation (US-067). One source serves every variant and both targets: the variant enters as build-time defines
// (MG_BLEND_MODE, MG_TWO_SIDED), and the shader reads its instance parameters from a constant buffer.
//
// What is baked and what is not. Constants, the topology, and the names and types of parameters and textures are in the source,
// so changing them changes the source hash and therefore the cache key. Parameter values and defaults are not in the source: an
// instance changes its values by rewriting the constant buffer (pack_parameters), with no shader rebuild.
//
// Bindings. Set 1 is the material: b0 is the parameter block (MgParams), and t1.. are the textures in texture_slots() order. The
// pixel entry PSGraph takes TEXCOORD0 and writes three targets: color and alpha, the encoded normal with roughness, and emissive with
// occlusion. The surface is MgEvaluate(uv), which is the same function the CPU evaluator (evaluate.hpp) runs.

#include "engine/core/result.hpp"
#include "engine/material_graph/graph.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace engine::material_graph {

inline constexpr std::string_view kParameterBlockName = "MgParams";
inline constexpr std::uint32_t kMaterialSet = 1;
// The parameter block is binding 0 and the textures start at 1. The RHI keys a set's bindings by number, so the two classes must differ.
inline constexpr std::uint32_t kFirstTextureBinding = 1;

// One float4 row of the parameter block. Each row holds one parameter, starting at its x component.
struct ParameterEntry {
    std::string name;
    ValueType type = ValueType::Float;
    std::uint32_t row = 0;
    glm::vec4 default_value{0.0f};
};

struct ParameterLayout {
    std::vector<ParameterEntry> entries;  // row order: alpha_cutoff first, then the graph's parameters by name

    [[nodiscard]] std::uint32_t size_bytes() const noexcept {
        return 16u * static_cast<std::uint32_t>(entries.size());
    }
    [[nodiscard]] const ParameterEntry* find(std::string_view name) const noexcept;
};

// The block layout of a graph. The graph must be valid (analyze() reports no errors).
[[nodiscard]] ParameterLayout parameter_layout(const Graph& graph);

// Instance values by name: one number per component of the parameter's type.
using ParameterValues = std::map<std::string, std::vector<float>>;

// Every parameter's value: the instance's where it names one, the default otherwise. Fails on a name the layout does not have, a
// wrong number of components, or a non-finite number.
[[nodiscard]] core::Result<std::map<std::string, glm::vec4, std::less<>>, std::vector<GraphError>> resolve_parameters(
    const ParameterLayout& layout, const ParameterValues& values);

// The constant buffer bytes, size_bytes() long. Padding is zero.
[[nodiscard]] core::Result<std::vector<std::uint8_t>, std::vector<GraphError>> pack_parameters(const ParameterLayout& layout,
                                                                                              const ParameterValues& values);

// The declared textures in slot order (name order), so slot i is register t<i> in set 1.
[[nodiscard]] std::vector<TextureDecl> texture_slots(const Graph& graph);

struct GeneratedShader {
    std::string hlsl;
    std::string hash;  // SHA-256 (hex) of hlsl. The file is named by it, so the path changes exactly when the source does
    ParameterLayout parameters;
    std::vector<TextureDecl> textures;
};

// Generates the source of a valid graph. A graph with errors returns them unchanged.
[[nodiscard]] core::Result<GeneratedShader, std::vector<GraphError>> generate_hlsl(const Graph& graph);

}  // namespace engine::material_graph
