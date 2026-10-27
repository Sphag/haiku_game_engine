#pragma once

// The CPU reference of a material graph (US-067). It runs the graph the way the generated MgEvaluate does: the same pin conversions,
// the same fallbacks, nearest-texel repeat sampling, and the same clamping in the PBR output. The tests check graph meaning here,
// so a GPU run only has to agree with it.

#include "engine/core/result.hpp"
#include "engine/material_graph/codegen.hpp"
#include "engine/material_graph/graph.hpp"
#include "engine/render/material.hpp"

#include <glm/glm.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace engine::material_graph {

// The surface at one texel: the fields of MgSurface in the generated source.
struct MaterialSurface {
    glm::vec4 base_color{1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive{0.0f};
    float occlusion = 1.0f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};  // tangent space
    float alpha = 1.0f;                  // base_color.a * the alpha pin, clamped
};

class SurfaceEvaluator {
public:
    // Fails with the graph's errors, or with a value problem in `values`. The parameters are fixed at creation: a new instance
    // makes a new evaluator, as it makes a new parameter block.
    [[nodiscard]] static core::Result<SurfaceEvaluator, std::vector<GraphError>> create(const Graph& graph, const ParameterValues& values);

    // Binds the image of a declared texture; nullptr unbinds it. An unbound texture samples as its declared fallback. Returns the first
    // problem, or nothing. The image must outlive every evaluate() call.
    [[nodiscard]] std::optional<std::string> bind_texture(const std::string& name, const render::ImageRgba8* image);

    [[nodiscard]] MaterialSurface evaluate(glm::vec2 uv) const;

private:
    SurfaceEvaluator(Graph graph, Analysis analysis, std::map<std::string, glm::vec4, std::less<>> parameters);

    [[nodiscard]] glm::vec4 input(const std::vector<glm::vec4>& values, const Node& node, std::string_view pin, ValueType target) const;

    Graph graph_;
    Analysis analysis_;
    std::map<std::string, glm::vec4, std::less<>> parameters_;
    std::map<std::string, const render::ImageRgba8*, std::less<>> textures_;
};

}  // namespace engine::material_graph
