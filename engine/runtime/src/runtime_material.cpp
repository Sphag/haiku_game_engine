#include "engine/runtime/runtime_material.hpp"

#include "asset_converter/block_compression.hpp"
#include "engine/assets/texture_format.hpp"
#include "engine/material_graph/graph.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace engine::runtime {

namespace {

using Json = nlohmann::json;
namespace mg = material_graph;

// What a graph node amounts to for the fixed surface model: a factor, and the name of the texture it samples, if any.
struct Value {
    glm::vec4 factor{1.0f};
    std::optional<std::string> texture;
};

struct Evaluator {
    const mg::Graph& graph;
    const mg::Analysis& analysis;
    const Json& parameters;
    std::vector<std::string>& notes;

    Value eval(const std::string& id) const {
        const auto found = analysis.index.find(id);
        if (found == analysis.index.end()) {
            return {};
        }
        const mg::Node& node = graph.nodes[found->second];
        Value out;
        switch (node.kind) {
            case mg::NodeKind::Constant:
                out.factor = widen(node.value, analysis.types[found->second]);
                break;
            case mg::NodeKind::Parameter: {
                glm::vec4 value = node.value;
                const auto override_value = parameters.find(node.name);
                if (override_value != parameters.end() && override_value->is_array() && !override_value->empty() && override_value->size() <= 4) {
                    for (std::size_t i = 0; i < override_value->size(); ++i) {
                        if ((*override_value)[i].is_number()) {
                            value[static_cast<int>(i)] = (*override_value)[i].get<float>();
                        }
                    }
                }
                out.factor = widen(value, analysis.types[found->second]);
                break;
            }
            case mg::NodeKind::Uv:
                break;
            case mg::NodeKind::TextureSample:
                out.texture = node.name;
                break;
            case mg::NodeKind::Arithmetic: {
                const Value a = eval(pin(node, "a"));
                const Value b = eval(pin(node, "b"));
                out.texture = a.texture ? a.texture : b.texture;
                switch (node.op) {
                    case mg::ArithmeticOp::Multiply:
                        out.factor = a.factor * b.factor;
                        break;
                    case mg::ArithmeticOp::Divide:
                        out.factor = glm::vec4(b.factor.x != 0.0f ? a.factor.x / b.factor.x : 0.0f, b.factor.y != 0.0f ? a.factor.y / b.factor.y : 0.0f,
                                               b.factor.z != 0.0f ? a.factor.z / b.factor.z : 0.0f, b.factor.w != 0.0f ? a.factor.w / b.factor.w : 0.0f);
                        break;
                    case mg::ArithmeticOp::Add:
                        out.factor = a.factor + b.factor;
                        break;
                    case mg::ArithmeticOp::Subtract:
                        out.factor = a.factor - b.factor;
                        break;
                    case mg::ArithmeticOp::Min:
                        out.factor = glm::min(a.factor, b.factor);
                        break;
                    case mg::ArithmeticOp::Max:
                        out.factor = glm::max(a.factor, b.factor);
                        break;
                }
                if (node.op != mg::ArithmeticOp::Multiply && (a.texture || b.texture)) {
                    notes.push_back("node '" + node.id + "' (" + std::string(mg::to_string(node.op)) +
                                    ") mixes a texture with another value; the fixed surface model takes only a multiply, so the texture is left out");
                    out.texture.reset();
                }
                break;
            }
            case mg::NodeKind::NormalMap: {
                const Value texture = eval(pin(node, "texture"));
                out.texture = texture.texture;
                const std::string scale = pin(node, "scale");
                out.factor = scale.empty() ? glm::vec4(1.0f) : eval(scale).factor;
                break;
            }
            case mg::NodeKind::PbrOutput:
                break;
        }
        return out;
    }

    static std::string pin(const mg::Node& node, const char* name) {
        const auto it = node.inputs.find(name);
        return it == node.inputs.end() ? std::string() : it->second;
    }

    // A vec3 feeds a vec4 pin with w = 1, and a float splats, so the factor's unused components are made consistent with its type.
    static glm::vec4 widen(glm::vec4 value, mg::ValueType type) {
        switch (type) {
            case mg::ValueType::Float:
                return glm::vec4(value.x);
            case mg::ValueType::Vec2:
                return glm::vec4(value.x, value.y, 1.0f, 1.0f);
            case mg::ValueType::Vec3:
                return glm::vec4(value.x, value.y, value.z, 1.0f);
            case mg::ValueType::Vec4:
                break;
        }
        return value;
    }
};

}  // namespace

core::Result<MaterialRecipe, std::string> resolve_material(std::string_view instance_text, const gameplay::AssetReader& reader) {
    const Json instance = Json::parse(instance_text, nullptr, false);
    if (instance.is_discarded() || !instance.is_object()) {
        return core::failure(std::string("the material is not a JSON object"));
    }
    static const std::vector<std::string> known{"format", "version", "graph", "textures", "parameters", "blend", "two_sided"};
    for (auto it = instance.begin(); it != instance.end(); ++it) {
        if (std::find(known.begin(), known.end(), it.key()) == known.end()) {
            return core::failure("the material has an unknown key '" + it.key() + "'");
        }
    }
    if (!instance.contains("format") || instance["format"] != "engine.material" || !instance.contains("version") || instance["version"] != 1) {
        return core::failure(std::string("the file is not an engine.material, version 1"));
    }
    if (!instance.contains("graph") || !instance["graph"].is_string()) {
        return core::failure(std::string("the material names no graph"));
    }
    const auto graph_guid = core::Guid::parse(instance["graph"].get<std::string>());
    if (!graph_guid.ok()) {
        return core::failure(std::string("the material's graph is not a GUID"));
    }
    const std::optional<std::string> graph_text = reader ? reader(graph_guid.value()) : std::nullopt;
    if (!graph_text) {
        return core::failure("the graph " + graph_guid.value().to_string() + " is not in the packs");
    }
    auto parsed = mg::parse_graph(*graph_text);
    if (!parsed.ok()) {
        return core::failure("the material graph: " + mg::to_text(parsed.error().front()));
    }
    const mg::Graph& graph = parsed.value();
    const mg::Analysis analysis = mg::analyze(graph);
    if (!analysis.errors.empty()) {
        return core::failure("the material graph: " + mg::to_text(analysis.errors.front()));
    }

    // The textures the instance binds, by the graph's names. A binding for a name the graph does not declare is a mistake worth reporting.
    std::map<std::string, core::Guid> bound;
    if (instance.contains("textures")) {
        if (!instance["textures"].is_object()) {
            return core::failure(std::string("textures must be an object of texture name to asset GUID"));
        }
        for (auto it = instance["textures"].begin(); it != instance["textures"].end(); ++it) {
            const bool declared = std::any_of(graph.textures.begin(), graph.textures.end(), [&](const mg::TextureDecl& d) { return d.name == it.key(); });
            const auto guid = it.value().is_string() ? core::Guid::parse(it.value().get<std::string>()) : core::Guid::parse("");
            if (!declared || !guid.ok()) {
                return core::failure("the texture '" + it.key() + "' is not declared by the graph, or is not a GUID");
            }
            bound[it.key()] = guid.value();
        }
    }

    MaterialRecipe recipe;
    static const Json no_parameters = Json::object();
    const Json& parameters = instance.contains("parameters") && instance["parameters"].is_object() ? instance["parameters"] : no_parameters;
    const Evaluator evaluator{graph, analysis, parameters, recipe.notes};
    const mg::Node& output = graph.nodes[analysis.output];

    const auto bind_texture = [&](const Value& value, const char* slot) {
        if (!value.texture) {
            return;
        }
        const auto found = bound.find(*value.texture);
        if (found == bound.end()) {
            recipe.notes.push_back("the graph samples '" + *value.texture + "', which the material does not bind; the default is used");
            return;
        }
        recipe.textures[slot] = found->second;
    };
    const auto pin_value = [&](const char* name) -> std::optional<Value> {
        const std::string id = Evaluator::pin(output, name);
        if (id.empty()) {
            return std::nullopt;
        }
        return evaluator.eval(id);
    };

    if (const auto base = pin_value("base_color")) {
        recipe.params.base_color = base->factor;
        bind_texture(*base, kSlotBaseColor);
    }
    if (const auto metallic = pin_value("metallic")) {
        recipe.params.metallic = metallic->factor.x;
        bind_texture(*metallic, kSlotMetallicRoughness);
    }
    if (const auto roughness = pin_value("roughness")) {
        recipe.params.roughness = roughness->factor.x;
        bind_texture(*roughness, kSlotMetallicRoughness);
    }
    if (const auto emissive = pin_value("emissive")) {
        recipe.params.emissive = glm::vec3(emissive->factor);
        bind_texture(*emissive, kSlotEmissive);
    }
    if (const auto occlusion = pin_value("occlusion")) {
        recipe.params.occlusion_strength = occlusion->factor.x;
        bind_texture(*occlusion, kSlotOcclusion);
    }
    if (const auto normal = pin_value("normal")) {
        recipe.params.normal_scale = normal->factor.x;
        bind_texture(*normal, kSlotNormal);
    }
    if (const auto alpha = pin_value("alpha")) {
        recipe.params.base_color.w *= alpha->factor.x;
    }

    if (instance.contains("blend")) {
        const std::string blend = instance["blend"].is_string() ? instance["blend"].get<std::string>() : std::string();
        if (blend == "opaque") {
            recipe.params.blend = render::BlendMode::Opaque;
        } else if (blend == "mask") {
            recipe.params.blend = render::BlendMode::AlphaMask;
        } else if (blend == "blend") {
            recipe.params.blend = render::BlendMode::AlphaBlend;
        } else {
            return core::failure(std::string("blend must be opaque, mask, or blend"));
        }
    }
    if (instance.contains("two_sided") && instance["two_sided"].is_boolean()) {
        recipe.params.two_sided = instance["two_sided"].get<bool>();
    }
    return core::success(std::move(recipe));
}

core::Result<render::ImageRgba8, std::string> decode_texture_payload(std::string_view payload) {
    auto parsed = assets::read_texture_payload(payload);
    if (!parsed.ok()) {
        return core::failure("the texture: " + parsed.error());
    }
    const assets::TexturePayload& texture = parsed.value();
    const assets::TextureMip& mip = texture.mips.front();
    render::ImageRgba8 image;
    image.width = mip.width;
    image.height = mip.height;
    image.pixels.assign(static_cast<std::size_t>(mip.width) * mip.height * 4, 255);

    if (texture.encoding == assets::TextureEncoding::Rgba8) {
        std::copy(mip.data.begin(), mip.data.begin() + static_cast<std::ptrdiff_t>(image.pixels.size()), image.pixels.begin());
        return core::success(std::move(image));
    }

    // Block-compressed: each 4x4 block decodes to 16 texels, and the ones past the image edge are dropped.
    const std::uint32_t blocks_x = (mip.width + 3) / 4;
    const std::uint32_t blocks_y = (mip.height + 3) / 4;
    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            const auto* block = reinterpret_cast<const std::uint8_t*>(mip.data.data()) + (static_cast<std::size_t>(by) * blocks_x + bx) * converter::kBlockBytes;
            std::uint8_t texels[converter::kBlockTexels * 4];
            if (texture.encoding == assets::TextureEncoding::Bc7) {
                if (!converter::decode_bc7_block(block, texels)) {
                    return core::failure(std::string("a BC7 block is not valid"));
                }
            } else {
                converter::decode_bc5_block(block, texels);
                // BC5 stores X and Y of a unit vector; Z is rebuilt and the vector is written as n * 0.5 + 0.5.
                for (int t = 0; t < converter::kBlockTexels; ++t) {
                    const float x = texels[t * 4 + 0] / 255.0f * 2.0f - 1.0f;
                    const float y = texels[t * 4 + 1] / 255.0f * 2.0f - 1.0f;
                    const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                    texels[t * 4 + 2] = static_cast<std::uint8_t>(std::clamp(z * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            for (std::uint32_t ty = 0; ty < 4; ++ty) {
                for (std::uint32_t tx = 0; tx < 4; ++tx) {
                    const std::uint32_t x = bx * 4 + tx;
                    const std::uint32_t y = by * 4 + ty;
                    if (x < mip.width && y < mip.height) {
                        std::copy_n(texels + (ty * 4 + tx) * 4, 4, image.pixels.begin() + (static_cast<std::size_t>(y) * mip.width + x) * 4);
                    }
                }
            }
        }
    }
    return core::success(std::move(image));
}

}  // namespace engine::runtime
