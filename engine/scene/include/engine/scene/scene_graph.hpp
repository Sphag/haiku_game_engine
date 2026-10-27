#pragma once

#include "engine/scene/transform.hpp"

#include "engine/core/result.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/registry.hpp"

#include <cstddef>
#include <vector>

namespace engine::scene {

enum class HierarchyError {
    NotAlive,          // the handle is stale, foreign, or invalid
    Cycle,             // the new parent is the node itself or one of its descendants
    InvalidTransform,  // the local transform has a non-finite value or a non-unit rotation
    SingularParent,    // keeping a world transform needs the inverse of a singular parent matrix
    NotRepresentable   // keeping a world transform needs shear or a zero scale axis, which Transform cannot hold
};

// Keep the node's world transform (its local transform is recomputed for the new parent), or keep its local transform
// (the node moves with the new parent).
enum class ReparentMode { KeepWorldTransform, KeepLocalTransform };

// What happens to the children of a destroyed node.
enum class ChildPolicy {
    DestroySubtree,   // the children and all of their descendants are destroyed
    ReparentToParent  // the children move to the destroyed node's parent (or become roots) and keep their world transforms
};

// Object hierarchy of a scene. Each node is an ECS entity. Its components are the authored local Transform, the parent
// link with the child list, and a cached world matrix. Destroying a node removes its components with it.
//
// Dirty propagation: changing a local transform marks the node and its descendants dirty. A dirty node's descendants are
// always dirty, so marking stops at a node that is already dirty. world_matrix() refreshes one node and its dirty
// ancestors. update() refreshes every dirty node.
//
// Not thread-safe: the owning thread changes the graph at a synchronization point (ECS command buffers, US-014).
class SceneGraph {
public:
    using Status = core::Result<core::Unit, HierarchyError>;

    // Creates a node under parent, or a root when parent is not valid. The new node is dirty.
    [[nodiscard]] core::Result<ecs::EntityHandle, HierarchyError> create(const Transform& initial = {},
                                                                         ecs::EntityHandle parent_node = {});

    // Destroys a node and applies the policy to its children. On failure nothing changes. ReparentToParent fails with
    // NotRepresentable or SingularParent when a child's world transform cannot be kept as a Transform.
    [[nodiscard]] Status destroy(ecs::EntityHandle node, ChildPolicy policy);

    // Moves a node under new_parent, or makes it a root when new_parent is not valid. Fails with Cycle when new_parent
    // is the node or one of its descendants. On failure nothing changes.
    [[nodiscard]] Status reparent(ecs::EntityHandle node, ecs::EntityHandle new_parent, ReparentMode mode);

    // Replaces a node's local transform and marks the node and its descendants dirty.
    [[nodiscard]] Status set_local(ecs::EntityHandle node, const Transform& value);

    [[nodiscard]] bool alive(ecs::EntityHandle node) const noexcept;

    // Parent of a node. Returns an invalid handle for a root or for a node that is not alive.
    [[nodiscard]] ecs::EntityHandle parent(ecs::EntityHandle node) const noexcept;

    // Children in insertion order. Returns nullptr for a node that is not alive.
    [[nodiscard]] const std::vector<ecs::EntityHandle>* children(ecs::EntityHandle node) const noexcept;

    // Authored local transform. Returns nullptr for a node that is not alive.
    [[nodiscard]] const Transform* local(ecs::EntityHandle node) const noexcept;

    // True when the node's cached world matrix is out of date. False for a node that is not alive.
    [[nodiscard]] bool dirty(ecs::EntityHandle node) const noexcept;

    // World matrix, refreshed on demand. Returns nullptr for a node that is not alive. The pointer is valid until the
    // next structural change or transform change.
    [[nodiscard]] const glm::dmat4* world_matrix(ecs::EntityHandle node);

    // Refreshes every dirty node and returns how many were recomputed.
    std::size_t update();

    [[nodiscard]] std::vector<ecs::EntityHandle> roots() const;

    [[nodiscard]] std::size_t size() const noexcept;

    // The registry that owns the nodes. Liveness checks (scripts, physics) read it; only the graph changes it.
    [[nodiscard]] const ecs::Registry& registry() const noexcept {
        return registry_;
    }
    [[nodiscard]] ecs::Registry& registry() noexcept {
        return registry_;
    }

private:
    struct Hierarchy {
        ecs::EntityHandle parent;
        std::vector<ecs::EntityHandle> children;
    };

    struct WorldState {
        glm::dmat4 matrix{1.0};
        bool dirty = true;
    };

    [[nodiscard]] bool is_self_or_ancestor(ecs::EntityHandle candidate, ecs::EntityHandle node) const noexcept;
    void attach(ecs::EntityHandle child, ecs::EntityHandle new_parent);
    void detach(ecs::EntityHandle child) noexcept;
    void mark_dirty(ecs::EntityHandle node);
    std::size_t refresh_chain(ecs::EntityHandle node);
    [[nodiscard]] core::Result<Transform, HierarchyError> local_keeping_world(ecs::EntityHandle node,
                                                                              ecs::EntityHandle new_parent);

    ecs::Registry registry_;
};

}  // namespace engine::scene
