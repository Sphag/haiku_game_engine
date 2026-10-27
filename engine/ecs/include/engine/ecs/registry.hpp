#pragma once

#include "engine/ecs/entity.hpp"
#include "engine/ecs/sparse_set.hpp"
#include "engine/ecs/world.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
namespace detail {

// Type-erased view of one component pool, so the registry can remove an entity from every pool and query by membership.
class PoolBase {
public:
    PoolBase() = default;
    PoolBase(const PoolBase&) = delete;
    PoolBase& operator=(const PoolBase&) = delete;
    virtual ~PoolBase() = default;

    virtual bool remove_entity(EntityHandle entity) = 0;
    [[nodiscard]] virtual bool contains(EntityHandle entity) const noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<EntityHandle>& entities() const noexcept = 0;
};

template <typename T>
class Pool final : public PoolBase {
public:
    bool remove_entity(EntityHandle entity) override {
        return set.remove(entity);
    }
    [[nodiscard]] bool contains(EntityHandle entity) const noexcept override {
        return set.contains(entity);
    }
    [[nodiscard]] std::size_t size() const noexcept override {
        return set.size();
    }
    [[nodiscard]] const std::vector<EntityHandle>& entities() const noexcept override {
        return set.entities();
    }

    SparseSet<T> set;
};

}  // namespace detail

// Owns a World and one SparseSet pool per component type. Entity lifetime and component lifetime stay together: destroy()
// removes every component of the entity before releasing its slot, so a reused slot never inherits old components.
//
// Component operations check the handle against the owned World, so a stale or foreign handle cannot add or read a
// component. The registry is not thread-safe; the owning thread makes structural changes at a synchronization point.
class Registry {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = default;
    Registry& operator=(Registry&&) = default;
    ~Registry() = default;

    [[nodiscard]] const World& world() const noexcept {
        return world_;
    }

    [[nodiscard]] EntityHandle create() {
        return world_.create();
    }

    [[nodiscard]] core::Result<EntityHandle, WorldError> create(const core::Guid& persistent_id) {
        return world_.create(persistent_id);
    }

    // Removes every component of the entity, then destroys the entity. Returns false for a stale, foreign, or invalid handle.
    bool destroy(EntityHandle entity) {
        if (!world_.alive(entity)) {
            return false;
        }
        for (const auto& entry : pools_) {
            entry.second->remove_entity(entity);
        }
        return world_.destroy(entity);
    }

    // Adds a component of type T to a live entity. Returns nullptr when the entity is not alive in this registry's world
    // or already has a T; the existing value is left unchanged.
    template <typename T, typename... Args>
    T* add(EntityHandle entity, Args&&... args) {
        if (!world_.alive(entity)) {
            return nullptr;
        }
        return pool<T>().set.emplace(entity, std::forward<Args>(args)...);
    }

    // Removes the entity's T. Returns false when the entity has no T.
    template <typename T>
    bool remove(EntityHandle entity) {
        detail::Pool<T>* found = find_pool<T>();
        return found != nullptr && found->set.remove(entity);
    }

    // Returns the entity's T, or nullptr when the handle is not alive here or the entity has no T.
    template <typename T>
    [[nodiscard]] T* get(EntityHandle entity) {
        if (!world_.alive(entity)) {
            return nullptr;
        }
        detail::Pool<T>* found = find_pool<T>();
        return found == nullptr ? nullptr : found->set.get(entity);
    }
    template <typename T>
    [[nodiscard]] const T* get(EntityHandle entity) const {
        if (!world_.alive(entity)) {
            return nullptr;
        }
        const detail::Pool<T>* found = find_pool<T>();
        return found == nullptr ? nullptr : found->set.get(entity);
    }

    template <typename T>
    [[nodiscard]] bool has(EntityHandle entity) const {
        return get<T>(entity) != nullptr;
    }

    // Number of entities that currently hold a T.
    template <typename T>
    [[nodiscard]] std::size_t count() const {
        const detail::Pool<T>* found = find_pool<T>();
        return found == nullptr ? 0 : found->set.size();
    }

    // Live entities that hold every listed component type (an intersection). Membership is the contract: result order is
    // not stable and must not be saved or relied on. An empty type list, a type that no entity has ever held, or a
    // type with no current members yields an empty result.
    template <typename... Ts>
    [[nodiscard]] std::vector<EntityHandle> query() const {
        if constexpr (sizeof...(Ts) == 0) {
            return {};
        } else {
            const std::array<const detail::PoolBase*, sizeof...(Ts)> pools{find_pool<Ts>()...};

            // Walk the smallest pool and test the others, so the cost follows the rarest component.
            const detail::PoolBase* smallest = nullptr;
            for (const detail::PoolBase* candidate : pools) {
                if (candidate == nullptr || candidate->size() == 0) {
                    return {};
                }
                if (smallest == nullptr || candidate->size() < smallest->size()) {
                    smallest = candidate;
                }
            }

            std::vector<EntityHandle> result;
            for (const EntityHandle entity : smallest->entities()) {
                const bool in_all = std::all_of(pools.begin(), pools.end(), [entity](const detail::PoolBase* candidate_pool) {
                    return candidate_pool->contains(entity);
                });
                if (in_all) {
                    result.push_back(entity);
                }
            }
            return result;
        }
    }

private:
    template <typename T>
    detail::Pool<T>& pool() {
        auto& slot = pools_[std::type_index(typeid(T))];
        if (!slot) {
            slot = std::make_unique<detail::Pool<T>>();
        }
        return *static_cast<detail::Pool<T>*>(slot.get());
    }

    template <typename T>
    detail::Pool<T>* find_pool() {
        const auto found = pools_.find(std::type_index(typeid(T)));
        return found == pools_.end() ? nullptr : static_cast<detail::Pool<T>*>(found->second.get());
    }
    template <typename T>
    const detail::Pool<T>* find_pool() const {
        const auto found = pools_.find(std::type_index(typeid(T)));
        return found == pools_.end() ? nullptr : static_cast<const detail::Pool<T>*>(found->second.get());
    }

    World world_;
    std::unordered_map<std::type_index, std::unique_ptr<detail::PoolBase>> pools_;
};

}  // namespace engine::ecs
