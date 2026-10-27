#include "engine/material_graph/graph.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace engine::material_graph {
namespace {

using Json = nlohmann::json;

bool is_identifier(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !(digit && i > 0)) {
            return false;
        }
    }
    return true;
}

std::optional<NodeKind> parse_kind(std::string_view text) {
    if (text == "constant") return NodeKind::Constant;
    if (text == "parameter") return NodeKind::Parameter;
    if (text == "uv") return NodeKind::Uv;
    if (text == "texture_sample") return NodeKind::TextureSample;
    if (text == "arithmetic") return NodeKind::Arithmetic;
    if (text == "normal_map") return NodeKind::NormalMap;
    if (text == "pbr_output") return NodeKind::PbrOutput;
    return std::nullopt;
}

std::optional<ValueType> parse_type(std::string_view text) {
    if (text == "float") return ValueType::Float;
    if (text == "vec2") return ValueType::Vec2;
    if (text == "vec3") return ValueType::Vec3;
    if (text == "vec4") return ValueType::Vec4;
    return std::nullopt;
}

std::optional<ArithmeticOp> parse_op(std::string_view text) {
    if (text == "add") return ArithmeticOp::Add;
    if (text == "subtract") return ArithmeticOp::Subtract;
    if (text == "multiply") return ArithmeticOp::Multiply;
    if (text == "divide") return ArithmeticOp::Divide;
    if (text == "min") return ArithmeticOp::Min;
    if (text == "max") return ArithmeticOp::Max;
    return std::nullopt;
}

std::optional<ColorSpace> parse_color_space(std::string_view text) {
    if (text == "srgb") return ColorSpace::Srgb;
    if (text == "linear") return ColorSpace::Linear;
    return std::nullopt;
}

std::optional<Fallback> parse_fallback(std::string_view text) {
    if (text == "white") return Fallback::White;
    if (text == "black") return Fallback::Black;
    if (text == "flat_normal") return Fallback::FlatNormal;
    return std::nullopt;
}

// Which fields a node of each kind may carry. "id" and "kind" are always allowed.
bool field_applies(NodeKind kind, std::string_view key) {
    if (key == "id" || key == "kind") {
        return true;
    }
    switch (kind) {
        case NodeKind::Constant:
            return key == "type" || key == "value";
        case NodeKind::Parameter:
            return key == "type" || key == "name" || key == "value";
        case NodeKind::Uv:
            return false;
        case NodeKind::TextureSample:
            return key == "name" || key == "inputs";
        case NodeKind::Arithmetic:
            return key == "op" || key == "inputs";
        case NodeKind::NormalMap:
        case NodeKind::PbrOutput:
            return key == "inputs";
    }
    return false;
}

GraphError graph_error(std::string message) {
    return GraphError{"", "", std::move(message)};
}

GraphError node_error(const Node& node, std::string message) {
    return GraphError{node.id, std::string(to_string(node.kind)), std::move(message)};
}

PinSpec pin(const char* name, ValueType type, bool required, glm::vec4 fallback, bool any_type = false) {
    PinSpec spec;
    spec.name = name;
    spec.type = type;
    spec.required = required;
    spec.any_type = any_type;
    spec.fallback = fallback;
    return spec;
}

// Reads `components(type)` finite numbers from a JSON array. Returns false when the shape or a value is wrong.
bool parse_components(const Json& json, ValueType type, glm::vec4& out) {
    if (!json.is_array() || json.size() != components(type)) {
        return false;
    }
    glm::vec4 value{0.0f};
    for (std::size_t i = 0; i < json.size(); ++i) {
        if (!json[i].is_number()) {
            return false;
        }
        const float x = json[i].get<float>();
        if (!std::isfinite(x)) {
            return false;
        }
        value[static_cast<int>(i)] = x;
    }
    out = value;
    return true;
}

Json components_json(const glm::vec4& value, ValueType type) {
    Json array = Json::array();
    for (std::uint32_t i = 0; i < components(type); ++i) {
        array.push_back(static_cast<double>(value[static_cast<int>(i)]));
    }
    return array;
}

const Json* field(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

// Reads one node. Problems go to `errors`, and the node is returned either way.
Node parse_node(const Json& json, std::size_t position, std::vector<GraphError>& errors) {
    Node node;
    const std::string where = "node at position " + std::to_string(position);
    if (!json.is_object()) {
        errors.push_back(graph_error(where + " is not an object"));
        return node;
    }
    const Json* id = field(json, "id");
    if (id == nullptr || !id->is_string()) {
        errors.push_back(graph_error(where + " has no string id"));
        return node;
    }
    node.id = id->get<std::string>();

    std::string kind_text;
    const auto fail = [&node, &kind_text, &errors](const std::string& message) {
        errors.push_back(GraphError{node.id, kind_text, message});
    };

    const Json* kind = field(json, "kind");
    if (kind == nullptr || !kind->is_string()) {
        errors.push_back(GraphError{node.id, "", "has no kind"});
        return node;
    }
    kind_text = kind->get<std::string>();
    const std::optional<NodeKind> parsed_kind = parse_kind(kind_text);
    if (!parsed_kind) {
        fail("unknown kind '" + kind_text + "'");
        return node;
    }
    node.kind = *parsed_kind;

    for (const auto& item : json.items()) {
        if (!field_applies(node.kind, item.key())) {
            fail("field '" + item.key() + "' does not apply to a " + kind_text + " node");
        }
    }

    const Json* type = field(json, "type");
    if (type != nullptr) {
        const std::optional<ValueType> parsed = type->is_string() ? parse_type(type->get<std::string>()) : std::nullopt;
        if (parsed) {
            node.type = *parsed;
        } else {
            fail("type must be float, vec2, vec3, or vec4");
        }
    } else if (node.kind == NodeKind::Constant) {
        fail("needs a type");
    }

    const Json* value = field(json, "value");
    if (value != nullptr) {
        if (!parse_components(*value, node.type, node.value)) {
            fail("value must be an array of " + std::to_string(components(node.type)) + " finite numbers");
        }
    } else if (node.kind == NodeKind::Constant) {
        fail("needs a value");
    }

    const Json* name = field(json, "name");
    if (name != nullptr) {
        if (name->is_string()) {
            node.name = name->get<std::string>();
        } else {
            fail("name must be a string");
        }
    } else if (node.kind == NodeKind::Parameter || node.kind == NodeKind::TextureSample) {
        fail("needs a name");
    }

    const Json* op = field(json, "op");
    if (op != nullptr) {
        const std::optional<ArithmeticOp> parsed = op->is_string() ? parse_op(op->get<std::string>()) : std::nullopt;
        if (parsed) {
            node.op = *parsed;
        } else {
            fail("op must be add, subtract, multiply, divide, min, or max");
        }
    } else if (node.kind == NodeKind::Arithmetic) {
        fail("needs an op");
    }

    const Json* inputs = field(json, "inputs");
    if (inputs != nullptr) {
        if (!inputs->is_object()) {
            fail("inputs must be an object");
        } else {
            for (const auto& item : inputs->items()) {
                if (!item.value().is_string()) {
                    fail("input '" + item.key() + "' must name a node");
                } else {
                    node.inputs[item.key()] = item.value().get<std::string>();
                }
            }
        }
    }
    return node;
}

// Depth-first post-order over the links. A link to a node still open closes a cycle, and that link is the error.
void visit_node(std::size_t node_index, const std::vector<Node>& nodes, const std::map<std::string, std::size_t, std::less<>>& index,
           std::vector<int>& marks, std::vector<std::size_t>& order, std::vector<GraphError>& errors, bool& cyclic) {
    marks[node_index] = 1;  // open
    for (const auto& [pin_name, source] : nodes[node_index].inputs) {
        const auto found = index.find(source);
        if (found == index.end()) {
            continue;
        }
        const std::size_t source_index = found->second;
        if (marks[source_index] == 1) {
            cyclic = true;
            errors.push_back(node_error(nodes[node_index], "input '" + pin_name + "' closes a cycle through node '" + source + "'"));
        } else if (marks[source_index] == 0) {
            visit_node(source_index, nodes, index, marks, order, errors, cyclic);
        }
    }
    marks[node_index] = 2;  // done
    order.push_back(node_index);
}

}  // namespace

std::string_view to_string(ValueType type) noexcept {
    switch (type) {
        case ValueType::Float:
            return "float";
        case ValueType::Vec2:
            return "vec2";
        case ValueType::Vec3:
            return "vec3";
        case ValueType::Vec4:
            return "vec4";
    }
    return "unknown";
}

std::uint32_t components(ValueType type) noexcept {
    switch (type) {
        case ValueType::Float:
            return 1;
        case ValueType::Vec2:
            return 2;
        case ValueType::Vec3:
            return 3;
        case ValueType::Vec4:
            return 4;
    }
    return 1;
}

std::string_view to_string(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Constant:
            return "constant";
        case NodeKind::Parameter:
            return "parameter";
        case NodeKind::Uv:
            return "uv";
        case NodeKind::TextureSample:
            return "texture_sample";
        case NodeKind::Arithmetic:
            return "arithmetic";
        case NodeKind::NormalMap:
            return "normal_map";
        case NodeKind::PbrOutput:
            return "pbr_output";
    }
    return "unknown";
}

std::string_view to_string(ArithmeticOp op) noexcept {
    switch (op) {
        case ArithmeticOp::Add:
            return "add";
        case ArithmeticOp::Subtract:
            return "subtract";
        case ArithmeticOp::Multiply:
            return "multiply";
        case ArithmeticOp::Divide:
            return "divide";
        case ArithmeticOp::Min:
            return "min";
        case ArithmeticOp::Max:
            return "max";
    }
    return "unknown";
}

std::string_view to_string(ColorSpace space) noexcept {
    return space == ColorSpace::Srgb ? "srgb" : "linear";
}

std::string_view to_string(Fallback fallback) noexcept {
    switch (fallback) {
        case Fallback::White:
            return "white";
        case Fallback::Black:
            return "black";
        case Fallback::FlatNormal:
            return "flat_normal";
    }
    return "white";
}

std::string to_text(const GraphError& error) {
    if (error.node.empty()) {
        return error.message;
    }
    std::string text = "node '" + error.node + "'";
    if (!error.kind.empty()) {
        text += " (" + error.kind + ")";
    }
    return text + ": " + error.message;
}

std::vector<PinSpec> pin_specs(NodeKind kind) {
    switch (kind) {
        case NodeKind::TextureSample:
            return {pin("uv", ValueType::Vec2, true, glm::vec4(0.0f))};
        case NodeKind::Arithmetic:
            return {pin("a", ValueType::Float, true, glm::vec4(0.0f), true), pin("b", ValueType::Float, true, glm::vec4(0.0f), true)};
        case NodeKind::NormalMap:
            return {pin("texture", ValueType::Vec4, true, glm::vec4(0.0f)), pin("scale", ValueType::Float, false, glm::vec4(1.0f))};
        case NodeKind::PbrOutput:
            return {
                pin("base_color", ValueType::Vec4, false, glm::vec4(1.0f)),
                pin("metallic", ValueType::Float, false, glm::vec4(0.0f)),
                pin("roughness", ValueType::Float, false, glm::vec4(0.5f, 0.0f, 0.0f, 0.0f)),
                pin("emissive", ValueType::Vec3, false, glm::vec4(0.0f)),
                pin("occlusion", ValueType::Float, false, glm::vec4(1.0f)),
                pin("alpha", ValueType::Float, false, glm::vec4(1.0f)),
                pin("normal", ValueType::Vec3, false, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)),
            };
        case NodeKind::Constant:
        case NodeKind::Parameter:
        case NodeKind::Uv:
            return {};
    }
    return {};
}

bool accepts(ValueType source, ValueType target) noexcept {
    return source == target || source == ValueType::Float || (source == ValueType::Vec3 && target == ValueType::Vec4);
}

Analysis analyze(const Graph& graph) {
    Analysis result;
    std::vector<GraphError>& errors = result.errors;
    const std::vector<Node>& nodes = graph.nodes;

    std::set<std::string, std::less<>> textures;
    for (const TextureDecl& texture : graph.textures) {
        if (!is_identifier(texture.name)) {
            errors.push_back(graph_error("texture name '" + texture.name + "' is not an identifier"));
        } else if (!textures.insert(texture.name).second) {
            errors.push_back(graph_error("texture '" + texture.name + "' is declared twice"));
        }
    }

    std::map<std::string, std::string, std::less<>> parameter_owner;
    // The index is complete before any link is checked, so a node may name a source that is listed after it. The first node with an
    // id owns it; a second node with the same id is an error at that node.
    std::map<std::string, std::size_t, std::less<>> id_counts;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (is_identifier(nodes[i].id)) {
            result.index.emplace(nodes[i].id, i);
            ++id_counts[nodes[i].id];
        }
    }

    std::size_t outputs = 0;
    std::size_t output_index = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Node& node = nodes[i];
        if (!is_identifier(node.id)) {
            errors.push_back(node_error(node, "id '" + node.id + "' is not an identifier"));
        } else if (id_counts[node.id] > 1) {
            errors.push_back(node_error(node, "id is used by more than one node"));
        }

        if (node.kind == NodeKind::Parameter) {
            if (!is_identifier(node.name)) {
                errors.push_back(node_error(node, "parameter name '" + node.name + "' is not an identifier"));
            } else if (node.name == kAlphaCutoffParameter) {
                errors.push_back(node_error(node, "parameter name 'alpha_cutoff' is reserved for every material's alpha cutoff"));
            } else {
                const auto owner = parameter_owner.find(node.name);
                if (owner != parameter_owner.end()) {
                    errors.push_back(node_error(node, "parameter '" + node.name + "' is already declared by node '" + owner->second + "'"));
                } else {
                    parameter_owner.emplace(node.name, node.id);
                }
            }
        } else if (node.kind == NodeKind::TextureSample) {
            if (!textures.contains(node.name)) {
                errors.push_back(node_error(node, "texture '" + node.name + "' is not declared"));
            }
        } else if (node.kind == NodeKind::PbrOutput) {
            if (outputs == 0) {
                output_index = i;
            }
            ++outputs;
        }

        // Every link names a real pin of this kind and a real node; every required pin is linked.
        const std::vector<PinSpec> specs = pin_specs(node.kind);
        for (const auto& [pin_name, source] : node.inputs) {
            const bool known = std::any_of(specs.begin(), specs.end(), [wanted = pin_name](const PinSpec& spec) { return spec.name == wanted; });
            if (!known) {
                errors.push_back(node_error(node, "unknown input '" + pin_name + "'"));
            }
            if (!result.index.contains(source)) {
                errors.push_back(node_error(node, "input '" + pin_name + "' names unknown node '" + source + "'"));
            }
        }
        for (const PinSpec& spec : specs) {
            if (spec.required && !node.inputs.contains(spec.name)) {
                errors.push_back(node_error(node, "input '" + spec.name + "' is not connected"));
            }
        }
    }

    if (outputs != 1) {
        errors.push_back(graph_error("the graph needs exactly one pbr_output node, found " + std::to_string(outputs)));
    }

    // Cycles first: type inference needs an order, and a cyclic graph has none.
    std::vector<int> marks(nodes.size(), 0);
    std::vector<std::size_t> order;
    order.reserve(nodes.size());
    bool cyclic = false;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (marks[i] == 0) {
            visit_node(i, nodes, result.index, marks, order, errors, cyclic);
        }
    }
    if (cyclic) {
        return result;
    }

    result.order = order;
    result.types.assign(nodes.size(), ValueType::Float);
    for (const std::size_t u : order) {
        const Node& node = nodes[u];
        const std::vector<PinSpec> specs = pin_specs(node.kind);

        // The source index and type of a linked pin, or nothing when the pin is not linked.
        const auto linked = [&node, &result](const std::string& pin_name) -> std::optional<std::size_t> {
            const auto input = node.inputs.find(pin_name);
            if (input == node.inputs.end()) {
                return std::nullopt;
            }
            const auto found = result.index.find(input->second);
            if (found == result.index.end()) {
                return std::nullopt;
            }
            return found->second;
        };

        for (const PinSpec& spec : specs) {
            const std::optional<std::size_t> source = linked(spec.name);
            if (!source || spec.any_type) {
                continue;
            }
            const ValueType have = result.types[*source];
            if (!accepts(have, spec.type)) {
                errors.push_back(node_error(node, "input '" + spec.name + "' expects " + std::string(to_string(spec.type)) + ", got " +
                                                      std::string(to_string(have))));
            }
        }

        switch (node.kind) {
            case NodeKind::Constant:
            case NodeKind::Parameter:
                result.types[u] = node.type;
                break;
            case NodeKind::Uv:
                result.types[u] = ValueType::Vec2;
                break;
            case NodeKind::TextureSample:
                result.types[u] = ValueType::Vec4;
                break;
            case NodeKind::NormalMap:
                result.types[u] = ValueType::Vec3;
                break;
            case NodeKind::PbrOutput:
                result.types[u] = ValueType::Vec4;
                break;
            case NodeKind::Arithmetic: {
                const std::optional<std::size_t> a = linked("a");
                const std::optional<std::size_t> b = linked("b");
                const ValueType ta = a ? result.types[*a] : ValueType::Float;
                const ValueType tb = b ? result.types[*b] : ValueType::Float;
                if (ta == tb || tb == ValueType::Float) {
                    result.types[u] = ta;
                } else if (ta == ValueType::Float) {
                    result.types[u] = tb;
                } else {
                    errors.push_back(node_error(node, "inputs 'a' (" + std::string(to_string(ta)) + ") and 'b' (" +
                                                          std::string(to_string(tb)) + ") must match, or one must be a float"));
                    result.types[u] = ta;
                }
                break;
            }
        }
    }

    if (outputs == 1) {
        result.output = output_index;
    }
    return result;
}

std::vector<GraphError> validate_graph(const Graph& graph) {
    return analyze(graph).errors;
}

core::Result<Graph, std::vector<GraphError>> parse_graph(std::string_view text) {
    Json doc;
    try {
        doc = Json::parse(std::string(text));
    } catch (const Json::exception& error) {
        return core::failure(std::vector<GraphError>{graph_error(std::string("the document is not JSON: ") + error.what())});
    }
    if (!doc.is_object()) {
        return core::failure(std::vector<GraphError>{graph_error("the document must be a JSON object")});
    }

    std::vector<GraphError> errors;
    for (const auto& item : doc.items()) {
        const std::string& key = item.key();
        if (key != "schema" && key != "version" && key != "name" && key != "textures" && key != "nodes") {
            errors.push_back(graph_error("unknown field '" + key + "'"));
        }
    }

    const Json* schema = field(doc, "schema");
    if (schema == nullptr || !schema->is_string() || schema->get<std::string>() != kSchemaName) {
        errors.push_back(graph_error("schema must be '" + std::string(kSchemaName) + "'"));
    }
    const Json* version = field(doc, "version");
    if (version == nullptr || !version->is_number_unsigned() || version->get<std::uint64_t>() != kFormatVersion) {
        const std::string found = version == nullptr ? "none" : version->dump();
        errors.push_back(graph_error("unsupported graph version " + found + "; this build reads version " +
                                     std::to_string(kFormatVersion)));
    }
    if (!errors.empty()) {
        return core::failure(std::move(errors));
    }

    Graph graph;
    if (const Json* name = field(doc, "name")) {
        if (name->is_string()) {
            graph.name = name->get<std::string>();
        } else {
            errors.push_back(graph_error("name must be a string"));
        }
    }

    if (const Json* textures = field(doc, "textures")) {
        if (!textures->is_array()) {
            errors.push_back(graph_error("textures must be an array"));
        } else {
            for (const Json& entry : *textures) {
                TextureDecl decl;
                const Json* decl_name = entry.is_object() ? field(entry, "name") : nullptr;
                const Json* space = entry.is_object() ? field(entry, "color_space") : nullptr;
                const Json* fallback = entry.is_object() ? field(entry, "fallback") : nullptr;
                if (decl_name == nullptr || !decl_name->is_string()) {
                    errors.push_back(graph_error("a texture needs a string name"));
                    continue;
                }
                decl.name = decl_name->get<std::string>();
                const std::optional<ColorSpace> parsed =
                    space != nullptr && space->is_string() ? parse_color_space(space->get<std::string>()) : std::nullopt;
                if (!parsed) {
                    errors.push_back(graph_error("texture '" + decl.name + "' needs color_space 'srgb' or 'linear'"));
                    continue;
                }
                decl.color_space = *parsed;
                if (fallback != nullptr) {
                    const std::optional<Fallback> chosen =
                        fallback->is_string() ? parse_fallback(fallback->get<std::string>()) : std::nullopt;
                    if (!chosen) {
                        errors.push_back(graph_error("texture '" + decl.name + "' fallback must be white, black, or flat_normal"));
                        continue;
                    }
                    decl.fallback = *chosen;
                }
                graph.textures.push_back(decl);
            }
        }
    }

    const Json* nodes = field(doc, "nodes");
    if (nodes == nullptr || !nodes->is_array()) {
        errors.push_back(graph_error("nodes must be an array"));
    } else {
        for (std::size_t i = 0; i < nodes->size(); ++i) {
            graph.nodes.push_back(parse_node((*nodes)[i], i, errors));
        }
    }

    if (!errors.empty()) {
        return core::failure(std::move(errors));
    }
    errors = validate_graph(graph);
    if (!errors.empty()) {
        return core::failure(std::move(errors));
    }
    return core::success(std::move(graph));
}

std::string graph_to_json(const Graph& graph) {
    Json doc;
    doc["schema"] = std::string(kSchemaName);
    doc["version"] = graph.version;
    doc["name"] = graph.name;

    Json textures = Json::array();
    for (const TextureDecl& texture : graph.textures) {
        Json entry;
        entry["name"] = texture.name;
        entry["color_space"] = std::string(to_string(texture.color_space));
        entry["fallback"] = std::string(to_string(texture.fallback));
        textures.push_back(entry);
    }
    doc["textures"] = textures;

    Json nodes = Json::array();
    for (const Node& node : graph.nodes) {
        Json entry;
        entry["id"] = node.id;
        entry["kind"] = std::string(to_string(node.kind));
        switch (node.kind) {
            case NodeKind::Constant:
                entry["type"] = std::string(to_string(node.type));
                entry["value"] = components_json(node.value, node.type);
                break;
            case NodeKind::Parameter:
                entry["type"] = std::string(to_string(node.type));
                entry["name"] = node.name;
                entry["value"] = components_json(node.value, node.type);
                break;
            case NodeKind::TextureSample:
                entry["name"] = node.name;
                break;
            case NodeKind::Arithmetic:
                entry["op"] = std::string(to_string(node.op));
                break;
            case NodeKind::Uv:
            case NodeKind::NormalMap:
            case NodeKind::PbrOutput:
                break;
        }
        if (!node.inputs.empty()) {
            Json inputs = Json::object();
            for (const auto& [pin_name, source] : node.inputs) {
                inputs[pin_name] = source;
            }
            entry["inputs"] = inputs;
        }
        nodes.push_back(entry);
    }
    doc["nodes"] = nodes;

    return doc.dump(2) + "\n";
}

}  // namespace engine::material_graph
