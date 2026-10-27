#include "engine/scene/scene_format.hpp"

#include "engine/core/file_io.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace engine::scene {
namespace {

using Json = nlohmann::json;

SceneError malformed(std::string message) {
    return SceneError{SceneErrorCode::Malformed, std::move(message)};
}

SceneError invalid_value(std::string message) {
    return SceneError{SceneErrorCode::InvalidValue, std::move(message)};
}

// The member of an object, or nullptr when it is absent.
const Json* member(const Json& object, const char* name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

// Rejects a key the format does not define. Unknown keys are errors, so a newer file is never read partly.
std::optional<SceneError> check_keys(const Json& object, std::initializer_list<const char*> allowed, const std::string& where) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const char* name : allowed) {
            known = known || it.key() == name;
        }
        if (!known) {
            return malformed("unknown key '" + it.key() + "' in " + where);
        }
    }
    return std::nullopt;
}

// The shortest decimal text that reads back as the same float, so 0.2 is written as 0.2 and not 0.200000003.
double shortest_float(float value) {
    char buffer[32] = "0";
    for (int digits = 1; digits <= 9; ++digits) {
        std::snprintf(buffer, sizeof buffer, "%.*g", digits, static_cast<double>(value));
        if (std::strtof(buffer, nullptr) == value) {
            break;
        }
    }
    return std::strtod(buffer, nullptr);
}

// Reads a JSON array of exactly N numbers.
template <std::size_t N>
std::optional<std::array<double, N>> number_array(const Json& json) {
    if (!json.is_array() || json.size() != N) {
        return std::nullopt;
    }
    std::array<double, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        if (!json[i].is_number()) {
            return std::nullopt;
        }
        out[i] = json[i].get<double>();
    }
    return out;
}

// Converts a JSON value to a field value of the given kind. Returns nullopt when the JSON type or range does not fit.
// Non-finite floats cannot occur in JSON, so only range and type are checked here; check_value checks the rest.
std::optional<FieldValue> value_from_json(const Json& json, FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool:
            if (json.is_boolean()) {
                return FieldValue(json.get<bool>());
            }
            return std::nullopt;
        case FieldKind::Int32: {
            // A non-negative integer parses as unsigned, so each spelling is range-checked on its own type.
            if (!json.is_number_integer()) {
                return std::nullopt;
            }
            if (json.is_number_unsigned()) {
                const auto wide = json.get<std::uint64_t>();
                if (wide > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                    return std::nullopt;
                }
                return FieldValue(static_cast<std::int32_t>(wide));
            }
            const auto wide = json.get<std::int64_t>();
            if (wide < std::numeric_limits<std::int32_t>::min()) {
                return std::nullopt;
            }
            return FieldValue(static_cast<std::int32_t>(wide));
        }
        case FieldKind::UInt32: {
            if (!json.is_number_integer()) {
                return std::nullopt;
            }
            std::uint64_t wide = 0;
            if (json.is_number_unsigned()) {
                wide = json.get<std::uint64_t>();
            } else {
                const auto signed_value = json.get<std::int64_t>();
                if (signed_value < 0) {
                    return std::nullopt;
                }
                wide = static_cast<std::uint64_t>(signed_value);
            }
            if (wide > std::numeric_limits<std::uint32_t>::max()) {
                return std::nullopt;
            }
            return FieldValue(static_cast<std::uint32_t>(wide));
        }
        case FieldKind::Float32:
            if (json.is_number()) {
                return FieldValue(static_cast<float>(json.get<double>()));
            }
            return std::nullopt;
        case FieldKind::Float32x2:
        case FieldKind::Float32x3:
        case FieldKind::Float32x4: {
            const std::size_t count = kind == FieldKind::Float32x2 ? 2 : kind == FieldKind::Float32x3 ? 3 : 4;
            if (!json.is_array() || json.size() != count) {
                return std::nullopt;
            }
            std::array<float, 4> values{};
            for (std::size_t i = 0; i < count; ++i) {
                if (!json[i].is_number()) {
                    return std::nullopt;
                }
                values[i] = static_cast<float>(json[i].get<double>());
            }
            if (count == 2) {
                return FieldValue(std::array<float, 2>{values[0], values[1]});
            }
            if (count == 3) {
                return FieldValue(std::array<float, 3>{values[0], values[1], values[2]});
            }
            return FieldValue(values);
        }
        case FieldKind::Guid: {
            if (!json.is_string()) {
                return std::nullopt;
            }
            const auto parsed = core::Guid::parse(json.get<std::string>());
            if (!parsed.ok()) {
                return std::nullopt;
            }
            return FieldValue(parsed.value());
        }
    }
    return std::nullopt;
}

struct ValueToJson {
    Json operator()(bool value) const {
        return value;
    }
    Json operator()(std::int32_t value) const {
        return value;
    }
    Json operator()(std::uint32_t value) const {
        return value;
    }
    Json operator()(float value) const {
        return shortest_float(value);
    }
    template <std::size_t N>
    Json operator()(const std::array<float, N>& values) const {
        Json out = Json::array();
        for (const float component : values) {
            out.push_back(shortest_float(component));
        }
        return out;
    }
    Json operator()(const core::Guid& value) const {
        return value.to_string();
    }
};

Json guid_json(const core::Guid& guid) {
    return guid.is_nil() ? Json(nullptr) : Json(guid.to_string());
}

Json transform_to_json(const Transform& transform) {
    Json out = Json::object();
    out["position"] = Json::array({transform.position.x, transform.position.y, transform.position.z});
    out["rotation"] = Json::array({transform.rotation.w, transform.rotation.x, transform.rotation.y, transform.rotation.z});
    out["scale"] = Json::array({transform.scale.x, transform.scale.y, transform.scale.z});
    return out;
}

// Each part of a transform is optional and defaults to the identity. Doubles are read exactly as written.
core::Result<Transform, SceneError> transform_from_json(const Json& json, const std::string& where) {
    if (!json.is_object()) {
        return core::failure(malformed(where + ": transform must be an object"));
    }
    if (auto unknown = check_keys(json, {"position", "rotation", "scale"}, where + " transform")) {
        return core::failure(*unknown);
    }
    Transform out;
    if (const Json* position = member(json, "position")) {
        const auto v = number_array<3>(*position);
        if (!v) {
            return core::failure(invalid_value(where + ": position must be three numbers"));
        }
        out.position = glm::dvec3((*v)[0], (*v)[1], (*v)[2]);
    }
    if (const Json* rotation = member(json, "rotation")) {
        const auto v = number_array<4>(*rotation);
        if (!v) {
            return core::failure(invalid_value(where + ": rotation must be four numbers (w, x, y, z)"));
        }
        out.rotation = glm::dquat((*v)[0], (*v)[1], (*v)[2], (*v)[3]);
    }
    if (const Json* scale = member(json, "scale")) {
        const auto v = number_array<3>(*scale);
        if (!v) {
            return core::failure(invalid_value(where + ": scale must be three numbers"));
        }
        out.scale = glm::dvec3((*v)[0], (*v)[1], (*v)[2]);
    }
    if (!out.is_valid()) {
        return core::failure(SceneError{SceneErrorCode::InvalidTransform, where + ": transform is not finite or its rotation is not unit length"});
    }
    return core::success(out);
}

// Reads a GUID member that must be a string. The nil GUID is refused: it means "no entity" and is written as null.
core::Result<core::Guid, SceneError> entity_guid(const Json& json, const std::string& where) {
    if (!json.is_string()) {
        return core::failure(malformed(where + ": GUID must be a string"));
    }
    const auto parsed = core::Guid::parse(json.get<std::string>());
    if (!parsed.ok()) {
        return core::failure(malformed(where + ": not a GUID: " + json.get<std::string>()));
    }
    if (parsed.value().is_nil()) {
        return core::failure(SceneError{SceneErrorCode::NilGuid, where + ": the nil GUID is not an entity identifier"});
    }
    return core::success(parsed.value());
}

core::Result<core::Guid, SceneError> parent_guid(const Json* json, const std::string& where) {
    if (json == nullptr || json->is_null()) {
        return core::success(core::Guid{});
    }
    if (!json->is_string()) {
        return core::failure(malformed(where + ": parent must be a GUID string or null"));
    }
    const auto parsed = core::Guid::parse(json->get<std::string>());
    if (!parsed.ok() || parsed.value().is_nil()) {
        return core::failure(malformed(where + ": parent must be a GUID string or null"));
    }
    return core::success(parsed.value());
}

core::Result<ComponentData, SceneError> component_from_json(const Json& json, const std::string& type_id,
                                                            const ComponentCatalog& catalog) {
    const std::string where = "component '" + type_id + "'";
    const ComponentDescriptor* descriptor = catalog.find(type_id);
    if (descriptor == nullptr) {
        return core::failure(SceneError{SceneErrorCode::UnknownComponent, where + " is not registered"});
    }
    if (!json.is_object()) {
        return core::failure(malformed(where + " must be an object"));
    }
    if (auto unknown = check_keys(json, {"schema", "fields"}, where)) {
        return core::failure(*unknown);
    }
    const Json* schema = member(json, "schema");
    if (schema == nullptr || !schema->is_number_integer()) {
        return core::failure(malformed(where + " needs an integer 'schema'"));
    }
    if (schema->get<std::int64_t>() != static_cast<std::int64_t>(descriptor->schema_version)) {
        return core::failure(SceneError{SceneErrorCode::UnsupportedComponentSchema,
                                        where + " has schema " + std::to_string(schema->get<std::int64_t>()) + ", the catalog has " +
                                            std::to_string(descriptor->schema_version)});
    }
    ComponentData data;
    data.schema_version = descriptor->schema_version;
    if (const Json* fields = member(json, "fields")) {
        if (!fields->is_object()) {
            return core::failure(malformed(where + ": 'fields' must be an object"));
        }
        for (auto it = fields->begin(); it != fields->end(); ++it) {
            const FieldDescriptor* field = descriptor->find_field(it.key());
            if (field == nullptr) {
                return core::failure(SceneError{SceneErrorCode::UnknownField, where + " has no field '" + it.key() + "'"});
            }
            const auto value = value_from_json(*it, field->kind());
            if (!value) {
                return core::failure(invalid_value(where + ": field '" + it.key() + "' has the wrong JSON type"));
            }
            data.fields[it.key()] = *value;
        }
    }
    std::map<std::string, ComponentData> single{{type_id, data}};
    if (auto problem = complete_components(single, catalog)) {
        return core::failure(*problem);
    }
    return core::success(single.at(type_id));
}

core::Result<PrefabOverride, SceneError> override_from_json(const Json& json, const ComponentCatalog& catalog) {
    if (!json.is_object()) {
        return core::failure(malformed("override must be an object"));
    }
    if (auto unknown = check_keys(json, {"entity", "component", "field", "value"}, "override")) {
        return core::failure(*unknown);
    }
    const Json* entity = member(json, "entity");
    const Json* component = member(json, "component");
    const Json* field = member(json, "field");
    const Json* value = member(json, "value");
    if (entity == nullptr || component == nullptr || field == nullptr || value == nullptr || !component->is_string() ||
        !field->is_string()) {
        return core::failure(malformed("override needs 'entity', 'component', 'field', and 'value'"));
    }
    auto guid = entity_guid(*entity, "override");
    if (!guid.ok()) {
        return core::failure(guid.error());
    }
    const std::string type_id = component->get<std::string>();
    const std::string field_name = field->get<std::string>();
    const ComponentDescriptor* descriptor = catalog.find(type_id);
    if (descriptor == nullptr) {
        return core::failure(SceneError{SceneErrorCode::UnknownComponent, "override names unregistered component '" + type_id + "'"});
    }
    const FieldDescriptor* declared = descriptor->find_field(field_name);
    if (declared == nullptr) {
        return core::failure(SceneError{SceneErrorCode::UnknownField, "override names unknown field '" + type_id + "." + field_name + "'"});
    }
    const auto parsed = value_from_json(*value, declared->kind());
    if (!parsed) {
        return core::failure(invalid_value("override '" + type_id + "." + field_name + "' has the wrong JSON type"));
    }
    return core::success(PrefabOverride{guid.value(), type_id, field_name, *parsed});
}

// Parses the "entities" array. Scenes may hold prefab instances; prefabs may not.
core::Result<std::vector<EntityRecord>, SceneError> entities_from_json(const Json& list, const ComponentCatalog& catalog,
                                                                       bool allow_instances) {
    if (!list.is_array()) {
        return core::failure(malformed("'entities' must be an array"));
    }
    std::vector<EntityRecord> out;
    out.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& json = list[i];
        const std::string where = "entity " + std::to_string(i);
        if (!json.is_object()) {
            return core::failure(malformed(where + " must be an object"));
        }
        if (auto unknown = check_keys(json, {"guid", "parent", "transform", "components", "prefab"}, where)) {
            return core::failure(*unknown);
        }
        const Json* guid_member = member(json, "guid");
        if (guid_member == nullptr) {
            return core::failure(malformed(where + " has no 'guid'"));
        }
        auto guid = entity_guid(*guid_member, where);
        if (!guid.ok()) {
            return core::failure(guid.error());
        }
        const std::string named = where + " " + guid.value().to_string();
        auto parent = parent_guid(member(json, "parent"), named);
        if (!parent.ok()) {
            return core::failure(parent.error());
        }
        EntityRecord record;
        record.guid = guid.value();
        record.parent = parent.value();
        if (const Json* transform = member(json, "transform")) {
            auto parsed = transform_from_json(*transform, named);
            if (!parsed.ok()) {
                return core::failure(parsed.error());
            }
            record.transform = parsed.value();
        }
        const Json* components = member(json, "components");
        const Json* prefab = member(json, "prefab");
        if (prefab != nullptr) {
            if (!allow_instances) {
                return core::failure(malformed(named + ": a prefab cannot contain a prefab instance"));
            }
            if (components != nullptr) {
                return core::failure(malformed(named + ": a prefab instance has no components of its own"));
            }
            if (!prefab->is_object()) {
                return core::failure(malformed(named + ": 'prefab' must be an object"));
            }
            if (auto unknown = check_keys(*prefab, {"guid", "overrides"}, named + " prefab")) {
                return core::failure(*unknown);
            }
            const Json* prefab_guid = member(*prefab, "guid");
            if (prefab_guid == nullptr) {
                return core::failure(malformed(named + ": prefab has no 'guid'"));
            }
            auto asset = entity_guid(*prefab_guid, named + " prefab");
            if (!asset.ok()) {
                return core::failure(asset.error());
            }
            PrefabReference reference{asset.value(), {}};
            if (const Json* overrides = member(*prefab, "overrides")) {
                if (!overrides->is_array()) {
                    return core::failure(malformed(named + ": 'overrides' must be an array"));
                }
                for (const Json& item : *overrides) {
                    auto parsed = override_from_json(item, catalog);
                    if (!parsed.ok()) {
                        return core::failure(parsed.error());
                    }
                    reference.overrides.push_back(parsed.value());
                }
            }
            record.prefab = std::move(reference);
        } else if (components != nullptr) {
            if (!components->is_object()) {
                return core::failure(malformed(named + ": 'components' must be an object"));
            }
            for (auto it = components->begin(); it != components->end(); ++it) {
                auto parsed = component_from_json(*it, it.key(), catalog);
                if (!parsed.ok()) {
                    return core::failure(parsed.error());
                }
                record.components[it.key()] = parsed.value();
            }
        }
        out.push_back(std::move(record));
    }
    return core::success(std::move(out));
}

// Checks the format name and version of the document, and that the top-level keys are the ones the format defines.
std::optional<SceneError> check_header(const Json& doc, std::string_view format) {
    if (!doc.is_object()) {
        return malformed("document must be a JSON object");
    }
    const Json* name = member(doc, "format");
    if (name == nullptr || !name->is_string()) {
        return SceneError{SceneErrorCode::WrongFormat, "document has no format name"};
    }
    if (name->get<std::string>() != std::string(format)) {
        return SceneError{SceneErrorCode::WrongFormat, "not a " + std::string(format) + " file: format is '" + name->get<std::string>() + "'"};
    }
    const Json* version = member(doc, "version");
    if (version == nullptr || !version->is_number_integer()) {
        return malformed("document has no integer version");
    }
    if (version->get<std::int64_t>() != kSceneFormatVersion) {
        return SceneError{SceneErrorCode::UnsupportedVersion, "version " + std::to_string(version->get<std::int64_t>()) +
                                                                  " is not supported; this build reads version " +
                                                                  std::to_string(kSceneFormatVersion)};
    }
    return std::nullopt;
}

Json entity_to_json(const EntityRecord& entity) {
    Json out = Json::object();
    out["guid"] = entity.guid.to_string();
    out["parent"] = guid_json(entity.parent);
    out["transform"] = transform_to_json(entity.transform);
    if (entity.prefab) {
        Json reference = Json::object();
        reference["guid"] = entity.prefab->prefab.to_string();
        Json overrides = Json::array();
        for (const PrefabOverride& item : entity.prefab->overrides) {
            Json entry = Json::object();
            entry["entity"] = item.entity.to_string();
            entry["component"] = item.component;
            entry["field"] = item.field;
            entry["value"] = std::visit(ValueToJson{}, item.value);
            overrides.push_back(std::move(entry));
        }
        reference["overrides"] = std::move(overrides);
        out["prefab"] = std::move(reference);
    } else {
        Json components = Json::object();
        for (const auto& [type_id, data] : entity.components) {
            Json fields = Json::object();
            for (const auto& [name, value] : data.fields) {
                fields[name] = std::visit(ValueToJson{}, value);
            }
            Json component = Json::object();
            component["schema"] = data.schema_version;
            component["fields"] = std::move(fields);
            components[type_id] = std::move(component);
        }
        out["components"] = std::move(components);
    }
    return out;
}

std::string entities_text(std::string_view format, const std::vector<EntityRecord>& entities) {
    Json doc = Json::object();
    doc["format"] = std::string(format);
    doc["version"] = kSceneFormatVersion;
    Json list = Json::array();
    for (const EntityRecord& entity : entities) {
        list.push_back(entity_to_json(entity));
    }
    doc["entities"] = std::move(list);
    return doc.dump(2) + "\n";
}

core::Result<Json, SceneError> parse_json(std::string_view text) {
    const Json doc = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        return core::failure(malformed("not valid JSON"));
    }
    return core::success(doc);
}

SceneError io_error(const core::IoError& error) {
    return SceneError{SceneErrorCode::Io, core::describe(error)};
}

}  // namespace

std::string_view to_string(SceneErrorCode code) noexcept {
    switch (code) {
        case SceneErrorCode::Io:
            return "Io";
        case SceneErrorCode::Malformed:
            return "Malformed";
        case SceneErrorCode::WrongFormat:
            return "WrongFormat";
        case SceneErrorCode::UnsupportedVersion:
            return "UnsupportedVersion";
        case SceneErrorCode::NilGuid:
            return "NilGuid";
        case SceneErrorCode::DuplicateGuid:
            return "DuplicateGuid";
        case SceneErrorCode::MissingParent:
            return "MissingParent";
        case SceneErrorCode::Cycle:
            return "Cycle";
        case SceneErrorCode::InvalidParent:
            return "InvalidParent";
        case SceneErrorCode::InvalidTransform:
            return "InvalidTransform";
        case SceneErrorCode::UnknownComponent:
            return "UnknownComponent";
        case SceneErrorCode::UnsupportedComponentSchema:
            return "UnsupportedComponentSchema";
        case SceneErrorCode::UnknownField:
            return "UnknownField";
        case SceneErrorCode::InvalidValue:
            return "InvalidValue";
        case SceneErrorCode::InvalidPrefab:
            return "InvalidPrefab";
        case SceneErrorCode::MissingPrefab:
            return "MissingPrefab";
        case SceneErrorCode::InvalidOverride:
            return "InvalidOverride";
        case SceneErrorCode::UnknownEntity:
            return "UnknownEntity";
        case SceneErrorCode::InstanceEdit:
            return "InstanceEdit";
    }
    return "Unknown";
}

std::optional<SceneError> complete_components(std::map<std::string, ComponentData>& components, const ComponentCatalog& catalog) {
    for (auto& [type_id, data] : components) {
        const ComponentDescriptor* descriptor = catalog.find(type_id);
        if (descriptor == nullptr) {
            return SceneError{SceneErrorCode::UnknownComponent, "component '" + type_id + "' is not registered"};
        }
        if (data.schema_version != descriptor->schema_version) {
            return SceneError{SceneErrorCode::UnsupportedComponentSchema,
                              "component '" + type_id + "' has schema " + std::to_string(data.schema_version) +
                                  ", the catalog has " + std::to_string(descriptor->schema_version)};
        }
        for (const auto& [name, value] : data.fields) {
            const FieldDescriptor* field = descriptor->find_field(name);
            if (field == nullptr) {
                return SceneError{SceneErrorCode::UnknownField, "component '" + type_id + "' has no field '" + name + "'"};
            }
            if (auto problem = check_value(*field, value)) {
                return SceneError{SceneErrorCode::InvalidValue, "component '" + type_id + "' field '" + name + "': " + problem->message};
            }
        }
        for (const FieldDescriptor& field : descriptor->fields) {
            data.fields.try_emplace(field.name, field.default_value);
        }
    }
    return std::nullopt;
}

core::Result<SceneDocument, SceneError> parse_scene(std::string_view text, const ComponentCatalog& catalog) {
    auto parsed = parse_json(text);
    if (!parsed.ok()) {
        return core::failure(parsed.error());
    }
    const Json& doc = parsed.value();
    if (auto problem = check_header(doc, kSceneFormat)) {
        return core::failure(*problem);
    }
    if (auto unknown = check_keys(doc, {"format", "version", "entities"}, "scene")) {
        return core::failure(*unknown);
    }
    const Json* list = member(doc, "entities");
    if (list == nullptr) {
        return core::failure(malformed("scene has no 'entities'"));
    }
    auto entities = entities_from_json(*list, catalog, /*allow_instances=*/true);
    if (!entities.ok()) {
        return core::failure(entities.error());
    }
    return core::success(SceneDocument{std::move(entities.value())});
}

core::Result<PrefabDocument, SceneError> parse_prefab(std::string_view text, const ComponentCatalog& catalog) {
    auto parsed = parse_json(text);
    if (!parsed.ok()) {
        return core::failure(parsed.error());
    }
    const Json& doc = parsed.value();
    if (auto problem = check_header(doc, kPrefabFormat)) {
        return core::failure(*problem);
    }
    if (auto unknown = check_keys(doc, {"format", "version", "entities"}, "prefab")) {
        return core::failure(*unknown);
    }
    const Json* list = member(doc, "entities");
    if (list == nullptr) {
        return core::failure(malformed("prefab has no 'entities'"));
    }
    auto entities = entities_from_json(*list, catalog, /*allow_instances=*/false);
    if (!entities.ok()) {
        return core::failure(entities.error());
    }
    return core::success(PrefabDocument{std::move(entities.value())});
}

std::string serialize_scene(const SceneDocument& document) {
    return entities_text(kSceneFormat, document.entities);
}

std::string serialize_prefab(const PrefabDocument& document) {
    return entities_text(kPrefabFormat, document.entities);
}

core::Result<core::Unit, SceneError> save_scene(const core::Path& path, const SceneDocument& document) {
    const auto written = core::write_file_atomic(path, serialize_scene(document));
    if (!written.ok()) {
        return core::failure(io_error(written.error()));
    }
    return core::success();
}

core::Result<core::Unit, SceneError> save_prefab(const core::Path& path, const PrefabDocument& document) {
    const auto written = core::write_file_atomic(path, serialize_prefab(document));
    if (!written.ok()) {
        return core::failure(io_error(written.error()));
    }
    return core::success();
}

core::Result<SceneDocument, SceneError> load_scene(const core::Path& path, const ComponentCatalog& catalog) {
    const auto text = core::read_text_file(path);
    if (!text.ok()) {
        return core::failure(io_error(text.error()));
    }
    return parse_scene(text.value(), catalog);
}

core::Result<PrefabDocument, SceneError> load_prefab(const core::Path& path, const ComponentCatalog& catalog) {
    const auto text = core::read_text_file(path);
    if (!text.ok()) {
        return core::failure(io_error(text.error()));
    }
    return parse_prefab(text.value(), catalog);
}

}  // namespace engine::scene
