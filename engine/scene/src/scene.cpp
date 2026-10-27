#include "engine/scene/scene.hpp"

#include "engine/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <variant>

namespace engine::scene {
namespace {

SceneError error(SceneErrorCode code, std::string message) {
    return SceneError{code, std::move(message)};
}

// FNV-1a 64-bit over the bytes of two GUIDs, from a seed. Two seeds give the 128 bits of a derived GUID.
std::uint64_t fnv1a(const core::Guid& first, const core::Guid& second, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (const auto byte : first.bytes()) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    for (const auto byte : second.bytes()) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

// The GUID of a prefab-local entity inside one instance. It depends only on the two inputs, so a reference to it survives a
// save and load. It is shaped like a version 4 GUID, but it is not random.
core::Guid derive_guid(const core::Guid& instance, const core::Guid& local) {
    core::Guid::Bytes bytes{};
    const std::uint64_t high = fnv1a(instance, local, 0xcbf29ce484222325ULL);
    const std::uint64_t low = fnv1a(instance, local, 0x84222325cbf29ce4ULL);
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(high >> (56 - 8 * i));
        bytes[static_cast<std::size_t>(8 + i)] = static_cast<std::uint8_t>(low >> (56 - 8 * i));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return core::Guid(bytes);
}

// Checks a prefab template and returns it with defaults filled in. Requires one root, parents present, no cycles, no nested
// instances, valid transforms, and complete component values.
core::Result<PrefabDocument, SceneError> check_prefab(PrefabDocument prefab, const ComponentCatalog& catalog, core::Guid& root_out) {
    if (prefab.entities.empty()) {
        return core::failure(error(SceneErrorCode::InvalidPrefab, "prefab has no entities"));
    }
    std::unordered_map<core::Guid, std::size_t> index;
    std::size_t roots = 0;
    for (std::size_t i = 0; i < prefab.entities.size(); ++i) {
        EntityRecord& entity = prefab.entities[i];
        if (entity.guid.is_nil()) {
            return core::failure(error(SceneErrorCode::NilGuid, "prefab entity has the nil GUID"));
        }
        if (!index.emplace(entity.guid, i).second) {
            return core::failure(error(SceneErrorCode::DuplicateGuid, "prefab entity " + entity.guid.to_string() + " appears twice"));
        }
        if (entity.prefab) {
            return core::failure(error(SceneErrorCode::InvalidPrefab, "a prefab cannot contain a prefab instance"));
        }
        if (!entity.transform.is_valid()) {
            return core::failure(
                error(SceneErrorCode::InvalidTransform, "prefab entity " + entity.guid.to_string() + " has an invalid transform"));
        }
        if (entity.parent.is_nil()) {
            ++roots;
            root_out = entity.guid;
        }
        if (auto problem = complete_components(entity.components, catalog)) {
            return core::failure(*problem);
        }
    }
    if (roots != 1) {
        return core::failure(error(SceneErrorCode::InvalidPrefab, "prefab needs exactly one root, found " + std::to_string(roots)));
    }
    std::unordered_map<core::Guid, std::vector<core::Guid>> children;
    for (const EntityRecord& entity : prefab.entities) {
        if (entity.parent.is_nil()) {
            continue;
        }
        if (!index.contains(entity.parent)) {
            return core::failure(error(SceneErrorCode::MissingParent, "prefab entity " + entity.guid.to_string() + " names a missing parent"));
        }
        children[entity.parent].push_back(entity.guid);
    }
    // Walk down from the root. An entity the walk never reaches sits in a cycle.
    std::size_t reached = 0;
    std::vector<core::Guid> frontier{root_out};
    while (!frontier.empty()) {
        const core::Guid current = frontier.back();
        frontier.pop_back();
        ++reached;
        if (const auto found = children.find(current); found != children.end()) {
            frontier.insert(frontier.end(), found->second.begin(), found->second.end());
        }
    }
    if (reached != prefab.entities.size()) {
        return core::failure(error(SceneErrorCode::Cycle, "prefab hierarchy contains a cycle"));
    }
    return core::success(std::move(prefab));
}

// The value a prefab gives one field of one entity, or nullptr when the prefab has no such entity, component, or field.
const FieldValue* template_value(const PrefabDocument& prefab, const core::Guid& local, const std::string& type_id,
                                 const std::string& field) {
    for (const EntityRecord& entity : prefab.entities) {
        if (entity.guid != local) {
            continue;
        }
        const auto component = entity.components.find(type_id);
        if (component == entity.components.end()) {
            return nullptr;
        }
        const auto value = component->second.fields.find(field);
        return value == component->second.fields.end() ? nullptr : &value->second;
    }
    return nullptr;
}

// Builds the entities of one prefab instance, with the root under `parent`. Overrides are checked against the prefab before
// anything is built. The result is in the prefab's entity order.
core::Result<std::vector<detail::Pending>, SceneError> expand_instance(const core::Guid& instance, const PrefabDocument& prefab,
                                                                       const core::Guid& root, const std::map<detail::OverrideKey, FieldValue>& overrides,
                                                                       const Transform& root_transform, const core::Guid& parent,
                                                                       const ComponentCatalog& catalog) {
    for (const auto& [key, value] : overrides) {
        const auto& [local, type_id, field_name] = key;
        const std::string name = "'" + type_id + "." + field_name + "' on entity " + local.to_string();
        const FieldValue* current = template_value(prefab, local, type_id, field_name);
        const ComponentDescriptor* descriptor = catalog.find(std::string_view(type_id));
        const FieldDescriptor* field = descriptor == nullptr ? nullptr : descriptor->find_field(field_name);
        if (current == nullptr || field == nullptr) {
            return core::failure(error(SceneErrorCode::InvalidOverride, "override " + name + " names no prefab field"));
        }
        if (auto problem = check_value(*field, value)) {
            return core::failure(error(SceneErrorCode::InvalidOverride, "override " + name + ": " + problem->message));
        }
    }
    // The root keeps the instance GUID, so its children name it as their parent.
    const auto resolve = [&](const core::Guid& local) { return local == root ? instance : derive_guid(instance, local); };
    std::vector<detail::Pending> batch;
    batch.reserve(prefab.entities.size());
    std::unordered_map<core::Guid, std::size_t> index;
    for (const EntityRecord& entity : prefab.entities) {
        const bool is_root = entity.guid == root;
        detail::Pending pending;
        pending.guid = resolve(entity.guid);
        pending.parent = is_root ? parent : resolve(entity.parent);
        pending.transform = is_root ? root_transform : entity.transform;
        pending.components = entity.components;
        pending.link = detail::PrefabLink{instance, entity.guid, is_root};
        index.emplace(entity.guid, batch.size());
        batch.push_back(std::move(pending));
    }
    for (const auto& [key, value] : overrides) {
        const auto& [local, type_id, field_name] = key;
        batch[index.at(local)].components[type_id].fields[field_name] = value;
    }
    return core::success(std::move(batch));
}

// Indices of the batch in an order where each entity follows its parent. A parent that is not in the batch counts as already
// created. Returns nullopt when some entity is never reached, which means a cycle.
std::optional<std::vector<std::size_t>> parents_first(const std::vector<detail::Pending>& batch,
                                                      const std::unordered_map<core::Guid, std::size_t>& index) {
    std::unordered_map<core::Guid, std::vector<std::size_t>> children;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        if (batch[i].parent.is_nil() || !index.contains(batch[i].parent)) {
            order.push_back(i);
        } else {
            children[batch[i].parent].push_back(i);
        }
    }
    for (std::size_t next = 0; next < order.size(); ++next) {
        if (const auto found = children.find(batch[order[next]].guid); found != children.end()) {
            order.insert(order.end(), found->second.begin(), found->second.end());
        }
    }
    if (order.size() != batch.size()) {
        return std::nullopt;
    }
    return order;
}

}  // namespace

core::Guid instance_entity_guid(const core::Guid& instance, const core::Guid& local) {
    return derive_guid(instance, local);
}

core::Result<core::Unit, SceneError> PrefabLibrary::add(const core::Guid& asset, PrefabDocument prefab) {
    if (asset.is_nil()) {
        return core::failure(error(SceneErrorCode::NilGuid, "prefab asset GUID is the nil GUID"));
    }
    if (entries_.contains(asset)) {
        return core::failure(error(SceneErrorCode::DuplicateGuid, "prefab " + asset.to_string() + " is already in the library"));
    }
    core::Guid root;
    auto checked = check_prefab(std::move(prefab), *catalog_, root);
    if (!checked.ok()) {
        return core::failure(checked.error());
    }
    entries_.emplace(asset, Entry{std::move(checked.value()), root});
    return core::success();
}

const PrefabDocument* PrefabLibrary::find(const core::Guid& asset) const {
    const auto found = entries_.find(asset);
    return found == entries_.end() ? nullptr : &found->second.document;
}

std::optional<core::Guid> PrefabLibrary::root_of(const core::Guid& asset) const {
    const auto found = entries_.find(asset);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    return found->second.root;
}

Scene::Scene(const ComponentCatalog& catalog, const PrefabLibrary& prefabs)
    : catalog_(&catalog), prefabs_(&prefabs), graph_(std::make_unique<SceneGraph>()) {}

core::Result<core::Unit, SceneError> Scene::load(const SceneDocument& document, const std::unordered_set<core::Guid>& known_assets) {
    // Everything is built in a staged scene. This scene changes only once the staged one has been created in full.
    Scene staged(*catalog_, *prefabs_);
    staged.known_assets_ = known_assets;
    std::vector<detail::Pending> batch;
    std::vector<std::pair<core::Guid, detail::InstanceRecord>> instances;
    for (const EntityRecord& entity : document.entities) {
        if (!entity.transform.is_valid()) {
            return core::failure(error(SceneErrorCode::InvalidTransform, "entity " + entity.guid.to_string() + " has an invalid transform"));
        }
        if (!entity.prefab) {
            std::map<std::string, ComponentData> components = entity.components;
            if (auto problem = complete_components(components, *catalog_)) {
                return core::failure(*problem);
            }
            batch.push_back(detail::Pending{entity.guid, entity.parent, entity.transform, std::move(components), std::nullopt});
            continue;
        }
        if (!entity.components.empty()) {
            return core::failure(error(SceneErrorCode::InvalidPrefab, "prefab instance " + entity.guid.to_string() + " has components of its own"));
        }
        const PrefabDocument* prefab = prefabs_->find(entity.prefab->prefab);
        const auto root = prefabs_->root_of(entity.prefab->prefab);
        if (prefab == nullptr || !root) {
            return core::failure(error(SceneErrorCode::MissingPrefab, "entity " + entity.guid.to_string() + " names prefab " +
                                                                          entity.prefab->prefab.to_string() + ", which is not in the library"));
        }
        detail::InstanceRecord record{entity.prefab->prefab, *root, {}};
        for (const PrefabOverride& item : entity.prefab->overrides) {
            if (!record.overrides.emplace(detail::OverrideKey{item.entity, item.component, item.field}, item.value).second) {
                return core::failure(error(SceneErrorCode::InvalidOverride, "override '" + item.component + "." + item.field + "' on entity " +
                                                                                item.entity.to_string() + " is repeated"));
            }
        }
        auto expanded = expand_instance(entity.guid, *prefab, *root, record.overrides, entity.transform, entity.parent, *catalog_);
        if (!expanded.ok()) {
            return core::failure(expanded.error());
        }
        for (detail::Pending& pending : expanded.value()) {
            batch.push_back(std::move(pending));
        }
        instances.emplace_back(entity.guid, std::move(record));
    }
    auto inserted = staged.insert(std::move(batch), std::move(instances));
    if (!inserted.ok()) {
        return core::failure(inserted.error());
    }
    *this = std::move(staged);
    return core::success();
}

SceneDocument Scene::to_document() const {
    SceneDocument document;
    for (const auto& [guid, node] : nodes_) {
        if (node.link && !node.link->root) {
            continue;  // inner entities of an instance come from the prefab and are not written
        }
        EntityRecord record;
        record.guid = guid;
        record.parent = node.parent;
        record.transform = *graph_->local(node.handle);
        if (node.link) {
            const detail::InstanceRecord& instance = instances_.at(node.link->instance);
            PrefabReference reference{instance.prefab, {}};
            for (const auto& [key, value] : instance.overrides) {
                reference.overrides.push_back(PrefabOverride{std::get<0>(key), std::get<1>(key), std::get<2>(key), value});
            }
            record.prefab = std::move(reference);
        } else {
            record.components = node.components;
        }
        document.entities.push_back(std::move(record));
    }
    return document;
}

core::Result<core::Guid, SceneError> Scene::add_entity(const Transform& local, const core::Guid& parent_node) {
    if (!local.is_valid()) {
        return core::failure(error(SceneErrorCode::InvalidTransform, "new entity has an invalid transform"));
    }
    const core::Guid guid = core::Guid::generate();
    auto inserted = insert({detail::Pending{guid, parent_node, local, {}, std::nullopt}}, {});
    if (!inserted.ok()) {
        return core::failure(inserted.error());
    }
    return core::success(guid);
}

core::Result<core::Guid, SceneError> Scene::instantiate(const core::Guid& prefab, const Transform& local, const core::Guid& parent_node) {
    const PrefabDocument* document = prefabs_->find(prefab);
    const auto root = prefabs_->root_of(prefab);
    if (document == nullptr || !root) {
        return core::failure(error(SceneErrorCode::MissingPrefab, "prefab " + prefab.to_string() + " is not in the library"));
    }
    if (!local.is_valid()) {
        return core::failure(error(SceneErrorCode::InvalidTransform, "instance has an invalid transform"));
    }
    const core::Guid instance = core::Guid::generate();
    const std::map<detail::OverrideKey, FieldValue> no_overrides;
    auto expanded = expand_instance(instance, *document, *root, no_overrides, local, parent_node, *catalog_);
    if (!expanded.ok()) {
        return core::failure(expanded.error());
    }
    detail::InstanceRecord record{prefab, *root, {}};
    std::vector<std::pair<core::Guid, detail::InstanceRecord>> instances;
    instances.emplace_back(instance, std::move(record));
    auto inserted = insert(std::move(expanded.value()), std::move(instances));
    if (!inserted.ok()) {
        return core::failure(inserted.error());
    }
    return core::success(instance);
}

core::Result<core::Unit, SceneError> Scene::add_component(const core::Guid& entity, std::string_view type_id) {
    Node* node = find_node(entity);
    if (node == nullptr) {
        return core::failure(error(SceneErrorCode::UnknownEntity, "entity " + entity.to_string() + " is not in the scene"));
    }
    if (node->link) {
        return core::failure(error(SceneErrorCode::InstanceEdit, "a prefab instance cannot gain components in version 1"));
    }
    const ComponentDescriptor* descriptor = catalog_->find(type_id);
    if (descriptor == nullptr) {
        return core::failure(error(SceneErrorCode::UnknownComponent, "component '" + std::string(type_id) + "' is not registered"));
    }
    if (node->components.contains(std::string(type_id))) {
        return core::success();
    }
    ComponentData data;
    data.schema_version = descriptor->schema_version;
    for (const FieldDescriptor& field : descriptor->fields) {
        data.fields.emplace(field.name, field.default_value);
    }
    node->components.emplace(std::string(type_id), std::move(data));
    return core::success();
}

core::Result<core::Unit, SceneError> Scene::set_field(const core::Guid& entity, std::string_view type_id, std::string_view field_name,
                                                      const FieldValue& value) {
    Node* node = find_node(entity);
    if (node == nullptr) {
        return core::failure(error(SceneErrorCode::UnknownEntity, "entity " + entity.to_string() + " is not in the scene"));
    }
    const std::string type(type_id);
    const std::string name(field_name);
    const auto component = node->components.find(type);
    if (component == node->components.end()) {
        return core::failure(error(SceneErrorCode::UnknownComponent, "entity " + entity.to_string() + " has no component '" + type + "'"));
    }
    const ComponentDescriptor* descriptor = catalog_->find(type_id);
    const FieldDescriptor* declared = descriptor == nullptr ? nullptr : descriptor->find_field(name);
    if (declared == nullptr) {
        return core::failure(error(SceneErrorCode::UnknownField, "component '" + type + "' has no field '" + name + "'"));
    }
    if (auto problem = check_value(*declared, value)) {
        return core::failure(error(SceneErrorCode::InvalidValue, "'" + type + "." + name + "': " + problem->message));
    }
    if (node->link) {
        detail::InstanceRecord& instance = instances_.at(node->link->instance);
        const PrefabDocument* prefab = prefabs_->find(instance.prefab);
        const FieldValue* base = prefab == nullptr ? nullptr : template_value(*prefab, node->link->local, type, name);
        const detail::OverrideKey key{node->link->local, type, name};
        if (base != nullptr && *base == value) {
            instance.overrides.erase(key);
        } else {
            instance.overrides[key] = value;
        }
    }
    component->second.fields[name] = value;
    return core::success();
}

core::Result<core::Unit, SceneError> Scene::set_transform(const core::Guid& entity, const Transform& local) {
    Node* node = find_node(entity);
    if (node == nullptr) {
        return core::failure(error(SceneErrorCode::UnknownEntity, "entity " + entity.to_string() + " is not in the scene"));
    }
    if (node->link && !node->link->root) {
        return core::failure(error(SceneErrorCode::InstanceEdit, "an entity inside a prefab instance takes its transform from the prefab"));
    }
    if (!local.is_valid()) {
        return core::failure(error(SceneErrorCode::InvalidTransform, "entity " + entity.to_string() + " would get an invalid transform"));
    }
    // The node is alive and the transform is valid, so the graph accepts the change.
    const auto status = graph_->set_local(node->handle, local);
    ENGINE_ASSERT(status.ok(), "a live node accepts a valid local transform");
    return core::success();
}

bool Scene::contains(const core::Guid& entity) const {
    return nodes_.contains(entity);
}

core::Guid Scene::parent(const core::Guid& entity) const {
    const Node* node = find_node(entity);
    return node == nullptr ? core::Guid{} : node->parent;
}

const Transform* Scene::transform(const core::Guid& entity) const {
    const Node* node = find_node(entity);
    return node == nullptr ? nullptr : graph_->local(node->handle);
}

std::optional<FieldValue> Scene::field(const core::Guid& entity, std::string_view type_id, std::string_view field_name) const {
    const Node* node = find_node(entity);
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto component = node->components.find(std::string(type_id));
    if (component == node->components.end()) {
        return std::nullopt;
    }
    const auto value = component->second.fields.find(std::string(field_name));
    if (value == component->second.fields.end()) {
        return std::nullopt;
    }
    return value->second;
}

std::vector<std::string> Scene::component_types(const core::Guid& entity) const {
    std::vector<std::string> types;
    const Node* node = find_node(entity);
    if (node == nullptr) {
        return types;
    }
    types.reserve(node->components.size());
    for (const auto& [type_id, data] : node->components) {
        types.push_back(type_id);
    }
    return types;
}

std::optional<core::Guid> Scene::prefab_of(const core::Guid& entity) const {
    const Node* node = find_node(entity);
    if (node == nullptr || !node->link) {
        return std::nullopt;
    }
    return instances_.at(node->link->instance).prefab;
}

bool Scene::is_overridden(const core::Guid& entity, std::string_view type_id, std::string_view field_name) const {
    const Node* node = find_node(entity);
    if (node == nullptr || !node->link) {
        return false;
    }
    const auto& overrides = instances_.at(node->link->instance).overrides;
    return overrides.contains(detail::OverrideKey{node->link->local, std::string(type_id), std::string(field_name)});
}

const glm::dmat4* Scene::world_matrix(const core::Guid& entity) {
    const Node* node = find_node(entity);
    return node == nullptr ? nullptr : graph_->world_matrix(node->handle);
}

std::size_t Scene::update() {
    return graph_->update();
}

std::vector<MissingReference> Scene::missing_references() const {
    std::vector<MissingReference> out;
    for (const auto& [guid, node] : nodes_) {
        for (const auto& [type_id, data] : node.components) {
            for (const auto& [name, value] : data.fields) {
                const auto* reference = std::get_if<core::Guid>(&value);
                if (reference != nullptr && !reference->is_nil() && !known_assets_.contains(*reference)) {
                    out.push_back(MissingReference{guid, type_id, name, *reference});
                }
            }
        }
    }
    return out;
}

core::Result<core::Unit, SceneError> Scene::insert(std::vector<detail::Pending> batch,
                                                   std::vector<std::pair<core::Guid, detail::InstanceRecord>> instances) {
    std::unordered_map<core::Guid, std::size_t> index;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        if (nodes_.contains(batch[i].guid) || !index.emplace(batch[i].guid, i).second) {
            return core::failure(error(SceneErrorCode::DuplicateGuid, "entity " + batch[i].guid.to_string() + " appears twice"));
        }
    }
    for (const detail::Pending& pending : batch) {
        if (pending.parent.is_nil()) {
            continue;
        }
        // A parent must be a plain entity. Entities inside an instance may name their parent inside it, and an instance root
        // may not be a child of an instance.
        bool parent_is_plain = false;
        if (const auto found = index.find(pending.parent); found != index.end()) {
            parent_is_plain = !batch[found->second].link;
        } else if (const Node* existing = find_node(pending.parent)) {
            parent_is_plain = !existing->link;
        } else {
            return core::failure(error(SceneErrorCode::MissingParent, "entity " + pending.guid.to_string() + " names a missing parent " +
                                                                          pending.parent.to_string()));
        }
        const bool inner = pending.link && !pending.link->root;
        if (!inner && !parent_is_plain) {
            return core::failure(error(SceneErrorCode::InvalidParent, "entity " + pending.guid.to_string() + " cannot be placed under a prefab instance"));
        }
    }
    const auto order = parents_first(batch, index);
    if (!order) {
        return core::failure(error(SceneErrorCode::Cycle, "the hierarchy contains a cycle"));
    }
    for (const std::size_t i : *order) {
        detail::Pending& pending = batch[i];
        ecs::EntityHandle parent_handle{};
        if (!pending.parent.is_nil()) {
            parent_handle = find_node(pending.parent)->handle;
        }
        // Every problem above was checked, so the graph accepts the node.
        const auto created = graph_->create(pending.transform, parent_handle);
        ENGINE_ASSERT(created.ok(), "a checked transform and a live parent create a node");
        nodes_.emplace(pending.guid, Node{created.value(), pending.guid, pending.parent, std::move(pending.components), pending.link});
    }
    for (auto& [root, record] : instances) {
        instances_.emplace(root, std::move(record));
    }
    return core::success();
}

Scene::Node* Scene::find_node(const core::Guid& entity) {
    const auto found = nodes_.find(entity);
    return found == nodes_.end() ? nullptr : &found->second;
}

const Scene::Node* Scene::find_node(const core::Guid& entity) const {
    const auto found = nodes_.find(entity);
    return found == nodes_.end() ? nullptr : &found->second;
}

std::vector<core::Guid> Scene::entity_guids() const {
    std::vector<core::Guid> guids;
    guids.reserve(nodes_.size());
    for (const auto& entry : nodes_) {
        guids.push_back(entry.first);
    }
    return guids;
}

std::optional<ecs::EntityHandle> Scene::handle(const core::Guid& entity) const {
    const Node* node = find_node(entity);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->handle;
}

}  // namespace engine::scene
