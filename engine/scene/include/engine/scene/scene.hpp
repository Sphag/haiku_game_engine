#pragma once

// Runtime scene (US-017): the entities of a document as SceneGraph nodes, their reflected component values, and prefab
// instances. Entities are addressed by persistent GUID; the graph's handles stay internal.
//
// Prefabs: a PrefabLibrary holds validated templates by asset GUID. An instance is one entity that names a prefab. It
// expands into nodes whose GUIDs are derived from the instance GUID and the prefab-local GUID, so a reference to an entity
// inside an instance is the same after a save and load. The scene stores the instance's overrides, not a copy of the
// prefab's values: setting a field on an instance node records an override only when the value differs from the prefab.
//
// Version 1 limits: a prefab instance cannot gain or lose components, and its inner entities cannot be moved, given new
// children, or re-transformed. Only the instance root has a transform of its own. Plain entities cannot be children of an
// instance or of its root.
//
// Missing asset references: a Guid field naming an asset not in the known set loads and is reported by missing_references().
// It is kept, so saving does not drop it. A missing prefab is an error, because the instance has no structure without it.
//
// Atomicity: load() checks the whole document before it changes the scene; a failed load leaves the previous contents
// in place. Other mutators check before they touch the graph.
//
// Not thread-safe: the owning thread changes the scene at a synchronization point.

#include "engine/scene/scene_format.hpp"
#include "engine/scene/scene_graph.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::scene {

// The GUID that a prefab-local entity has inside one instance. Use it to address an entity inside an instance, for example to
// read or override one of its fields. It is the same in every session for the same instance and prefab-local GUID.
[[nodiscard]] core::Guid instance_entity_guid(const core::Guid& instance, const core::Guid& local);

// Validated prefab templates keyed by asset GUID. A template is checked once, when it is added: exactly one root, every parent
// present and reachable, no cycles, no nested instances, valid transforms, and complete component values. The stored copy has
// its defaults filled in.
class PrefabLibrary {
public:
    explicit PrefabLibrary(const ComponentCatalog& catalog) : catalog_(&catalog) {}

    // Fails with DuplicateGuid when the asset is already present, and with InvalidPrefab or another error when the template is
    // not valid. On failure the library is unchanged.
    [[nodiscard]] core::Result<core::Unit, SceneError> add(const core::Guid& asset, PrefabDocument prefab);

    [[nodiscard]] const PrefabDocument* find(const core::Guid& asset) const;

    // The prefab-local GUID of the root. Empty when the asset is not in the library.
    [[nodiscard]] std::optional<core::Guid> root_of(const core::Guid& asset) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

private:
    struct Entry {
        PrefabDocument document;
        core::Guid root;
    };

    const ComponentCatalog* catalog_;
    std::map<core::Guid, Entry> entries_;
};

// A Guid field whose asset is not in the known set. `entity` is the GUID of the node that holds it.
struct MissingReference {
    core::Guid entity;
    std::string component;
    std::string field;
    core::Guid asset;
};

namespace detail {

struct PrefabLink {
    core::Guid instance;  // GUID of the instance root
    core::Guid local;     // GUID of this entity inside the prefab
    bool root = false;
};

// An entity before it is created in the graph. The transform is used once, to create the node.
struct Pending {
    core::Guid guid;
    core::Guid parent;
    Transform transform;
    std::map<std::string, ComponentData> components;
    std::optional<PrefabLink> link;
};

using OverrideKey = std::tuple<core::Guid, std::string, std::string>;  // prefab-local GUID, component, field

struct InstanceRecord {
    core::Guid prefab;
    core::Guid root;  // prefab-local GUID of the root
    std::map<OverrideKey, FieldValue> overrides;
};

}  // namespace detail

class Scene {
public:
    // The catalog and the library are not owned. Both must outlive the scene.
    Scene(const ComponentCatalog& catalog, const PrefabLibrary& prefabs);

    // Replaces the contents with the document's. A reference to an asset outside known_assets is kept and listed by
    // missing_references(). Fails with the first problem found, and then the previous contents remain.
    [[nodiscard]] core::Result<core::Unit, SceneError> load(const SceneDocument& document,
                                                            const std::unordered_set<core::Guid>& known_assets = {});

    // The scene as a document. Entities are in GUID order. Each prefab instance appears once, as its root, with its overrides.
    [[nodiscard]] SceneDocument to_document() const;

    // Adds a plain entity with a new GUID. A nil parent makes it a root.
    [[nodiscard]] core::Result<core::Guid, SceneError> add_entity(const Transform& local, const core::Guid& parent_node = {});

    // Instantiates a prefab under parent (nil for a root). The root's transform is the one given; the rest come from the prefab.
    [[nodiscard]] core::Result<core::Guid, SceneError> instantiate(const core::Guid& prefab, const Transform& local,
                                                                   const core::Guid& parent_node = {});

    // Adds a component with its reflected defaults. Adding one that is already present changes nothing.
    [[nodiscard]] core::Result<core::Unit, SceneError> add_component(const core::Guid& entity, std::string_view type_id);

    // Sets one field after checking it against the descriptor. On an instance node the value is recorded as an override, or
    // removed when it equals the prefab's value.
    [[nodiscard]] core::Result<core::Unit, SceneError> set_field(const core::Guid& entity, std::string_view type_id,
                                                                 std::string_view field_name, const FieldValue& value);

    // Replaces a transform. Allowed for plain entities and for instance roots only.
    [[nodiscard]] core::Result<core::Unit, SceneError> set_transform(const core::Guid& entity, const Transform& local);

    [[nodiscard]] bool contains(const core::Guid& entity) const;

    // Parent GUID. Nil for a root and for an entity that is not in the scene.
    [[nodiscard]] core::Guid parent(const core::Guid& entity) const;

    // Authored local transform, or nullptr for an entity that is not in the scene.
    [[nodiscard]] const Transform* transform(const core::Guid& entity) const;

    [[nodiscard]] std::optional<FieldValue> field(const core::Guid& entity, std::string_view type_id, std::string_view field_name) const;

    // The prefab an entity came from. Empty for a plain entity and for an unknown one.
    [[nodiscard]] std::optional<core::Guid> prefab_of(const core::Guid& entity) const;

    // Type IDs of the components an entity has, in sorted order. Empty for an unknown entity. An instance's inner entities list
    // the components they get from their prefab.
    [[nodiscard]] std::vector<std::string> component_types(const core::Guid& entity) const;

    // True when this instance node's field differs from its prefab value.
    [[nodiscard]] bool is_overridden(const core::Guid& entity, std::string_view type_id, std::string_view field_name) const;

    // World matrix, refreshed on demand. nullptr for an unknown entity.
    [[nodiscard]] const glm::dmat4* world_matrix(const core::Guid& entity);

    // Refreshes every dirty node and returns how many were recomputed.
    std::size_t update();

    [[nodiscard]] std::size_t size() const noexcept {
        return nodes_.size();
    }
    // The registry that holds the scene's entities. Scripts and physics read it to check liveness.
    [[nodiscard]] const ecs::Registry& registry() const noexcept {
        return graph_->registry();
    }
    // The same registry, writable, for systems that change components in place (scripts, physics). A transform changed this way must be
    // re-applied with set_transform() so the scene graph marks it and its descendants dirty.
    [[nodiscard]] ecs::Registry& mutable_registry() noexcept {
        return graph_->registry();
    }
    // Every entity's GUID in GUID order, including the entities a prefab instance expanded into.
    [[nodiscard]] std::vector<core::Guid> entity_guids() const;
    // The registry handle of a scene entity. Nullopt for an entity that is not in the scene.
    [[nodiscard]] std::optional<ecs::EntityHandle> handle(const core::Guid& entity) const;

    // Guid fields that name an asset outside the known set, in entity and field order.
    [[nodiscard]] std::vector<MissingReference> missing_references() const;

private:
    struct Node {
        ecs::EntityHandle handle;
        core::Guid guid;
        core::Guid parent;
        std::map<std::string, ComponentData> components;
        std::optional<detail::PrefabLink> link;
    };

    // Checks a batch against the scene and creates its nodes parents first. Nothing changes unless every check passes.
    [[nodiscard]] core::Result<core::Unit, SceneError> insert(std::vector<detail::Pending> batch,
                                                              std::vector<std::pair<core::Guid, detail::InstanceRecord>> instances);

    [[nodiscard]] Node* find_node(const core::Guid& entity);
    [[nodiscard]] const Node* find_node(const core::Guid& entity) const;

    const ComponentCatalog* catalog_;
    const PrefabLibrary* prefabs_;
    std::unique_ptr<SceneGraph> graph_;
    std::map<core::Guid, Node> nodes_;
    std::map<core::Guid, detail::InstanceRecord> instances_;  // keyed by instance root GUID
    std::unordered_set<core::Guid> known_assets_;
};

}  // namespace engine::scene
