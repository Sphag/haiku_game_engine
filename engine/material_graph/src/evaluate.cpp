#include "engine/material_graph/evaluate.hpp"

#include "engine/render/pbr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace engine::material_graph {
namespace {

// Converts a value between pin types, as codegen does: a float splats, a vec3 gets w = 1.
glm::vec4 convert(glm::vec4 value, ValueType from, ValueType to) {
    if (from == to) {
        return value;
    }
    if (from == ValueType::Float) {
        glm::vec4 splat(0.0f);
        for (int c = 0; c < static_cast<int>(components(to)); ++c) {
            splat[c] = value.x;
        }
        return splat;
    }
    value.w = 1.0f;
    return value;
}

// The HLSL saturate: NaN clamps to zero, as the comparisons below make it.
float saturate_value(float value) {
    return value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
}

glm::vec4 saturate_vec(glm::vec4 value) {
    return glm::vec4(saturate_value(value.x), saturate_value(value.y), saturate_value(value.z), saturate_value(value.w));
}

// Nearest texel with repeat addressing (FetchTexture in scene.hlsl). Row 0 is the top, so v = 0 is row 0.
glm::vec4 fetch(const render::ImageRgba8& image, glm::vec2 uv) {
    const float fu = uv.x - std::floor(uv.x);
    const float fv = uv.y - std::floor(uv.y);
    const std::uint32_t last_x = image.width - 1;
    const std::uint32_t last_y = image.height - 1;
    const auto x = std::min(static_cast<std::uint32_t>(std::floor(fu * static_cast<float>(image.width))), last_x);
    const auto y = std::min(static_cast<std::uint32_t>(std::floor(fv * static_cast<float>(image.height))), last_y);
    const std::uint8_t* texel = image.texel(x, y);
    return glm::vec4(static_cast<float>(texel[0]), static_cast<float>(texel[1]), static_cast<float>(texel[2]),
                     static_cast<float>(texel[3])) /
           255.0f;
}

// The value an unbound texture samples as: the renderer's default for an empty slot. The byte values are the 8-bit ones the GPU reads.
glm::vec4 fallback_color(Fallback fallback) {
    switch (fallback) {
        case Fallback::White:
            return glm::vec4(1.0f);
        case Fallback::Black:
            return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        case Fallback::FlatNormal:
            return glm::vec4(128.0f / 255.0f, 128.0f / 255.0f, 1.0f, 1.0f);
    }
    return glm::vec4(1.0f);
}

}  // namespace

SurfaceEvaluator::SurfaceEvaluator(Graph graph, Analysis analysis, std::map<std::string, glm::vec4, std::less<>> parameters)
    : graph_(std::move(graph)), analysis_(std::move(analysis)), parameters_(std::move(parameters)) {}

core::Result<SurfaceEvaluator, std::vector<GraphError>> SurfaceEvaluator::create(const Graph& graph, const ParameterValues& values) {
    Analysis analysis = analyze(graph);
    if (!analysis.errors.empty()) {
        return core::failure(std::move(analysis.errors));
    }
    auto resolved = resolve_parameters(parameter_layout(graph), values);
    if (!resolved.ok()) {
        return core::failure(std::move(resolved.error()));
    }
    return core::success(SurfaceEvaluator(graph, std::move(analysis), std::move(resolved.value())));
}

std::optional<std::string> SurfaceEvaluator::bind_texture(const std::string& name, const render::ImageRgba8* image) {
    const bool declared = std::any_of(graph_.textures.begin(), graph_.textures.end(),
                                      [&name](const TextureDecl& texture) { return texture.name == name; });
    if (!declared) {
        return "texture '" + name + "' is not declared by the graph";
    }
    if (image != nullptr) {
        if (auto problem = render::validate_image(*image)) {
            return problem;
        }
    }
    textures_[name] = image;
    return std::nullopt;
}

glm::vec4 SurfaceEvaluator::input(const std::vector<glm::vec4>& values, const Node& node, std::string_view pin, ValueType target) const {
    const std::vector<PinSpec> specs = pin_specs(node.kind);
    const auto spec = std::find_if(specs.begin(), specs.end(), [pin](const PinSpec& candidate) { return candidate.name == pin; });
    const auto linked = node.inputs.find(std::string(pin));
    if (linked == node.inputs.end()) {
        return convert(spec->fallback, spec->type, target);
    }
    const std::size_t source = analysis_.index.find(linked->second)->second;
    return convert(values[source], analysis_.types[source], target);
}

MaterialSurface SurfaceEvaluator::evaluate(glm::vec2 uv) const {
    std::vector<glm::vec4> values(graph_.nodes.size(), glm::vec4(0.0f));
    for (const std::size_t u : analysis_.order) {
        const Node& node = graph_.nodes[u];
        const ValueType type = analysis_.types[u];
        switch (node.kind) {
            case NodeKind::Constant:
                values[u] = node.value;
                break;
            case NodeKind::Parameter:
                values[u] = parameters_.at(node.name);
                break;
            case NodeKind::Uv:
                values[u] = glm::vec4(uv.x, uv.y, 0.0f, 0.0f);
                break;
            case NodeKind::TextureSample: {
                const glm::vec2 coord = glm::vec2(input(values, node, "uv", ValueType::Vec2));
                const auto declared = std::find_if(graph_.textures.begin(), graph_.textures.end(),
                                                   [&node](const TextureDecl& texture) { return texture.name == node.name; });
                glm::vec4 color = fallback_color(declared->fallback);
                const auto bound = textures_.find(node.name);
                if (bound != textures_.end() && bound->second != nullptr) {
                    color = fetch(*bound->second, coord);
                }
                if (declared->color_space == ColorSpace::Srgb) {
                    color.r = render::srgb_to_linear(color.r);
                    color.g = render::srgb_to_linear(color.g);
                    color.b = render::srgb_to_linear(color.b);
                }
                values[u] = color;
                break;
            }
            case NodeKind::Arithmetic: {
                const glm::vec4 a = input(values, node, "a", type);
                const glm::vec4 b = input(values, node, "b", type);
                glm::vec4 result(0.0f);
                for (int c = 0; c < static_cast<int>(components(type)); ++c) {
                    switch (node.op) {
                        case ArithmeticOp::Add:
                            result[c] = a[c] + b[c];
                            break;
                        case ArithmeticOp::Subtract:
                            result[c] = a[c] - b[c];
                            break;
                        case ArithmeticOp::Multiply:
                            result[c] = a[c] * b[c];
                            break;
                        case ArithmeticOp::Divide:
                            result[c] = a[c] / b[c];
                            break;
                        case ArithmeticOp::Min:
                            result[c] = std::min(a[c], b[c]);
                            break;
                        case ArithmeticOp::Max:
                            result[c] = std::max(a[c], b[c]);
                            break;
                    }
                }
                values[u] = result;
                break;
            }
            case NodeKind::NormalMap: {
                // The stored rgb is n * 0.5 + 0.5; xy scale by the pin, then the vector normalizes.
                const glm::vec4 texel = input(values, node, "texture", ValueType::Vec4);
                const float scale = input(values, node, "scale", ValueType::Float).x;
                const glm::vec3 t = glm::vec3(texel) * 2.0f - 1.0f;
                values[u] = glm::vec4(glm::normalize(glm::vec3(t.x * scale, t.y * scale, t.z)), 0.0f);
                break;
            }
            case NodeKind::PbrOutput:
                break;
        }
    }

    const Node& pbr = graph_.nodes[analysis_.output];
    MaterialSurface surface;
    surface.base_color = saturate_vec(input(values, pbr, "base_color", ValueType::Vec4));
    surface.metallic = saturate_value(input(values, pbr, "metallic", ValueType::Float).x);
    surface.roughness = saturate_value(input(values, pbr, "roughness", ValueType::Float).x);
    const glm::vec3 emissive = glm::vec3(input(values, pbr, "emissive", ValueType::Vec3));
    surface.emissive = glm::max(emissive, glm::vec3(0.0f));
    surface.occlusion = saturate_value(input(values, pbr, "occlusion", ValueType::Float).x);
    surface.normal = glm::vec3(input(values, pbr, "normal", ValueType::Vec3));
    surface.alpha = saturate_value(surface.base_color.a * input(values, pbr, "alpha", ValueType::Float).x);
    return surface;
}

}  // namespace engine::material_graph
