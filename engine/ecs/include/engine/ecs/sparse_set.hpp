#pragma once

#include "engine/ecs/entity.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace engine::ecs {

// Pool of one component type. A sparse array indexed by entity slot points into dense arrays of handles and components,
// so add, remove, get, and membership are O(1), and iteration reads only the dense arrays.
//
// Each dense entry keeps the full handle, not just the index. A handle with the same slot but another generation or
// world is not a member, so a stale handle never reaches a component that belongs to a newer entity.
//
// Removal moves the last element into the hole, which changes dense order. That order is not a save-format contract:
// callers compare membership, not position.
//
// Components are stored in std::vector, so alignment follows alignof(T). T must be move-constructible and
// move-assignable; it need not be copyable. The pool itself is move-only so that components are never duplicated.
template <typename T>
class SparseSet {
public:
    SparseSet() = default;
    SparseSet(const SparseSet&) = delete;
    SparseSet& operator=(const SparseSet&) = delete;
    SparseSet(SparseSet&&) noexcept = default;
    SparseSet& operator=(SparseSet&&) noexcept = default;
    ~SparseSet() = default;

    [[nodiscard]] bool contains(EntityHandle entity) const noexcept {
        return find_position(entity) != kNone;
    }

    // Adds a component to an entity that does not have one yet. Returns nullptr when the entity already has a component
    // of this type; the existing value is left unchanged. Any entry left at the same slot by a different (stale) handle
    // is removed first, so each slot maps to at most one entry.
    template <typename... Args>
    T* emplace(EntityHandle entity, Args&&... args) {
        if (contains(entity)) {
            return nullptr;
        }
        if (entity.index < sparse_.size() && sparse_[entity.index] != kNone) {
            remove(entities_[sparse_[entity.index]]);
        }
        if (entity.index >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(entity.index) + 1, kNone);
        }
        components_.emplace_back(std::forward<Args>(args)...);
        entities_.push_back(entity);
        sparse_[entity.index] = static_cast<std::uint32_t>(entities_.size() - 1);
        return &components_.back();
    }

    // Destroys the entity's component. Returns false when the entity is not a member.
    bool remove(EntityHandle entity) {
        const std::uint32_t position = find_position(entity);
        if (position == kNone) {
            return false;
        }
        const std::uint32_t last = static_cast<std::uint32_t>(entities_.size() - 1);
        if (position != last) {
            components_[position] = std::move(components_[last]);
            entities_[position] = entities_[last];
            sparse_[entities_[position].index] = position;
        }
        components_.pop_back();
        entities_.pop_back();
        sparse_[entity.index] = kNone;
        return true;
    }

    // Returns the entity's component, or nullptr when the entity is not a member.
    [[nodiscard]] T* get(EntityHandle entity) noexcept {
        const std::uint32_t position = find_position(entity);
        return position == kNone ? nullptr : &components_[position];
    }
    [[nodiscard]] const T* get(EntityHandle entity) const noexcept {
        const std::uint32_t position = find_position(entity);
        return position == kNone ? nullptr : &components_[position];
    }

    // Destroys every component and forgets every member. The pool stays usable.
    void clear() noexcept {
        for (const EntityHandle& entity : entities_) {
            sparse_[entity.index] = kNone;
        }
        components_.clear();
        entities_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entities_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return entities_.empty();
    }

    // Members in dense order. The order changes on removal and is not stable across edits.
    [[nodiscard]] const std::vector<EntityHandle>& entities() const noexcept {
        return entities_;
    }

private:
    static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] std::uint32_t find_position(EntityHandle entity) const noexcept {
        if (entity.index >= sparse_.size()) {
            return kNone;
        }
        const std::uint32_t position = sparse_[entity.index];
        return position != kNone && entities_[position] == entity ? position : kNone;
    }

    std::vector<std::uint32_t> sparse_;  // entity index -> dense position, or kNone
    std::vector<EntityHandle> entities_;  // dense: member handles, parallel to components_
    std::vector<T> components_;           // dense: component values
};

}  // namespace engine::ecs
