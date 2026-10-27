#include "engine/scene/scene_graph.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace engine::scene {
namespace {

// Same relative tolerance as decompose_exact: singularity is judged against the largest entry, with an absolute floor of 1.
constexpr double kInverseTolerance = 1e-9;

const glm::dmat4 kIdentity{1.0};

// Inverse of a parent's world matrix, or nullopt when the parent is singular. A singular matrix has no finite inverse, or
// its "inverse" does not multiply back to identity.
std::optional<glm::dmat4> invert_parent(const glm::dmat4& parent) {
    const glm::dmat4 inverse = glm::inverse(parent);
    double largest = 1.0;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(inverse[column][row])) {
                return std::nullopt;
            }
            largest = std::max(largest, std::abs(inverse[column][row]));
        }
    }
    const glm::dmat4 product = inverse * parent;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const double expected = column == row ? 1.0 : 0.0;
            if (std::abs(product[column][row] - expected) > kInverseTolerance * largest) {
                return std::nullopt;
            }
        }
    }
    return inverse;
}

}  // namespace

core::Result<ecs::EntityHandle, HierarchyError> SceneGraph::create(const Transform& initial, ecs::EntityHandle parent_node) {
    if (!initial.is_valid()) {
        return core::failure(HierarchyError::InvalidTransform);
    }
    if (parent_node.valid() && !alive(parent_node)) {
        return core::failure(HierarchyError::NotAlive);
    }
    const ecs::EntityHandle node = registry_.create();
    registry_.add<Transform>(node, initial);
    registry_.add<Hierarchy>(node);
    registry_.add<WorldState>(node);
    attach(node, parent_node);
    return core::success(node);
}

SceneGraph::Status SceneGraph::destroy(ecs::EntityHandle node, ChildPolicy policy) {
    if (!alive(node)) {
        return core::failure(HierarchyError::NotAlive);
    }

    if (policy == ChildPolicy::ReparentToParent) {
        // Compute every child's new local transform before changing anything, so a failure leaves the graph as it was.
        const ecs::EntityHandle grandparent = parent(node);
        const std::vector<ecs::EntityHandle> child_nodes = *children(node);
        std::vector<Transform> new_locals;
        new_locals.reserve(child_nodes.size());
        for (const ecs::EntityHandle child : child_nodes) {
            core::Result<Transform, HierarchyError> computed = local_keeping_world(child, grandparent);
            if (!computed.ok()) {
                return core::failure(computed.error());
            }
            new_locals.push_back(computed.value());
        }
        for (std::size_t i = 0; i < child_nodes.size(); ++i) {
            detach(child_nodes[i]);
            attach(child_nodes[i], grandparent);
            *registry_.get<Transform>(child_nodes[i]) = new_locals[i];
        }
    }

    detach(node);

    // Every node in the removed subtree. Under ReparentToParent the list is only the node itself, because its children
    // were moved above.
    std::vector<ecs::EntityHandle> doomed;
    std::vector<ecs::EntityHandle> pending{node};
    while (!pending.empty()) {
        const ecs::EntityHandle current = pending.back();
        pending.pop_back();
        doomed.push_back(current);
        if (const std::vector<ecs::EntityHandle>* below = children(current)) {
            pending.insert(pending.end(), below->begin(), below->end());
        }
    }
    for (const ecs::EntityHandle entity : doomed) {
        registry_.destroy(entity);
    }
    return core::success();
}

SceneGraph::Status SceneGraph::reparent(ecs::EntityHandle node, ecs::EntityHandle new_parent, ReparentMode mode) {
    if (!alive(node)) {
        return core::failure(HierarchyError::NotAlive);
    }
    if (new_parent.valid()) {
        if (!alive(new_parent)) {
            return core::failure(HierarchyError::NotAlive);
        }
        if (is_self_or_ancestor(node, new_parent)) {
            return core::failure(HierarchyError::Cycle);
        }
    }

    Transform next_local = *registry_.get<Transform>(node);
    if (mode == ReparentMode::KeepWorldTransform) {
        core::Result<Transform, HierarchyError> computed = local_keeping_world(node, new_parent);
        if (!computed.ok()) {
            return core::failure(computed.error());
        }
        next_local = computed.value();
    }

    detach(node);
    attach(node, new_parent);
    *registry_.get<Transform>(node) = next_local;
    return core::success();
}

SceneGraph::Status SceneGraph::set_local(ecs::EntityHandle node, const Transform& value) {
    if (!alive(node)) {
        return core::failure(HierarchyError::NotAlive);
    }
    if (!value.is_valid()) {
        return core::failure(HierarchyError::InvalidTransform);
    }
    *registry_.get<Transform>(node) = value;
    mark_dirty(node);
    return core::success();
}

bool SceneGraph::alive(ecs::EntityHandle node) const noexcept {
    return registry_.world().alive(node);
}

ecs::EntityHandle SceneGraph::parent(ecs::EntityHandle node) const noexcept {
    const Hierarchy* link = registry_.get<Hierarchy>(node);
    return link == nullptr ? ecs::EntityHandle{} : link->parent;
}

const std::vector<ecs::EntityHandle>* SceneGraph::children(ecs::EntityHandle node) const noexcept {
    const Hierarchy* link = registry_.get<Hierarchy>(node);
    return link == nullptr ? nullptr : &link->children;
}

const Transform* SceneGraph::local(ecs::EntityHandle node) const noexcept {
    return registry_.get<Transform>(node);
}

bool SceneGraph::dirty(ecs::EntityHandle node) const noexcept {
    const WorldState* state = registry_.get<WorldState>(node);
    return state != nullptr && state->dirty;
}

const glm::dmat4* SceneGraph::world_matrix(ecs::EntityHandle node) {
    if (!alive(node)) {
        return nullptr;
    }
    refresh_chain(node);
    return &registry_.get<WorldState>(node)->matrix;
}

std::size_t SceneGraph::update() {
    std::size_t recomputed = 0;
    for (const ecs::EntityHandle node : registry_.query<WorldState>()) {
        recomputed += refresh_chain(node);
    }
    return recomputed;
}

std::vector<ecs::EntityHandle> SceneGraph::roots() const {
    std::vector<ecs::EntityHandle> result;
    for (const ecs::EntityHandle node : registry_.query<Hierarchy>()) {
        if (!parent(node).valid()) {
            result.push_back(node);
        }
    }
    return result;
}

std::size_t SceneGraph::size() const noexcept {
    return registry_.world().size();
}

bool SceneGraph::is_self_or_ancestor(ecs::EntityHandle candidate, ecs::EntityHandle node) const noexcept {
    for (ecs::EntityHandle current = node; current.valid(); current = parent(current)) {
        if (current == candidate) {
            return true;
        }
    }
    return false;
}

void SceneGraph::attach(ecs::EntityHandle child, ecs::EntityHandle new_parent) {
    registry_.get<Hierarchy>(child)->parent = new_parent;
    if (new_parent.valid()) {
        registry_.get<Hierarchy>(new_parent)->children.push_back(child);
    }
    // The child's world matrix depends on its new parent, so the child and its subtree must be recomputed.
    mark_dirty(child);
}

void SceneGraph::detach(ecs::EntityHandle child) noexcept {
    Hierarchy* link = registry_.get<Hierarchy>(child);
    if (link == nullptr) {
        return;
    }
    if (Hierarchy* old_parent = registry_.get<Hierarchy>(link->parent)) {
        std::vector<ecs::EntityHandle>& siblings = old_parent->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
    }
    link->parent = {};
}

void SceneGraph::mark_dirty(ecs::EntityHandle node) {
    std::vector<ecs::EntityHandle> pending{node};
    while (!pending.empty()) {
        const ecs::EntityHandle current = pending.back();
        pending.pop_back();
        WorldState* state = registry_.get<WorldState>(current);
        // A dirty node already has a dirty subtree, so there is nothing below it to visit.
        if (state == nullptr || state->dirty) {
            continue;
        }
        state->dirty = true;
        if (const std::vector<ecs::EntityHandle>* below = children(current)) {
            pending.insert(pending.end(), below->begin(), below->end());
        }
    }
}

std::size_t SceneGraph::refresh_chain(ecs::EntityHandle node) {
    // Collect the dirty part of the path up from node. A clean node has only clean ancestors, so the walk stops there.
    std::vector<ecs::EntityHandle> chain;
    for (ecs::EntityHandle current = node; dirty(current); current = parent(current)) {
        chain.push_back(current);
    }

    // Recompute from the topmost dirty node down, so each parent is clean before its children read it.
    std::size_t recomputed = 0;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const ecs::EntityHandle current = *it;
        const ecs::EntityHandle parent_node = parent(current);
        const WorldState* parent_state = parent_node.valid() ? registry_.get<WorldState>(parent_node) : nullptr;
        const glm::dmat4& parent_matrix = parent_state == nullptr ? kIdentity : parent_state->matrix;

        WorldState* state = registry_.get<WorldState>(current);
        state->matrix = parent_matrix * registry_.get<Transform>(current)->matrix();
        state->dirty = false;
        ++recomputed;
    }
    return recomputed;
}

core::Result<Transform, HierarchyError> SceneGraph::local_keeping_world(ecs::EntityHandle node,
                                                                        ecs::EntityHandle new_parent) {
    glm::dmat4 relative = *world_matrix(node);
    if (new_parent.valid()) {
        const std::optional<glm::dmat4> inverse = invert_parent(*world_matrix(new_parent));
        if (!inverse) {
            return core::failure(HierarchyError::SingularParent);
        }
        relative = *inverse * relative;
    }

    const std::optional<Transform> decomposed = decompose_exact(relative);
    if (!decomposed) {
        return core::failure(HierarchyError::NotRepresentable);
    }
    return core::success(*decomposed);
}

}  // namespace engine::scene
