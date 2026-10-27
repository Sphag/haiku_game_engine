#pragma once

#include "engine/ecs/entity.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace engine::ecs {

enum class WorldError {
    NilGuid,       // a persistent id must be a real GUID; the nil value means "no id"
    DuplicateGuid  // another live entity in this world already has the persistent id
};

// Owns a set of entities. Worlds are isolated: each one has a process-unique id, handles carry that id, and a GUID
// is only meaningful inside the world that holds it. A World is move-only, because a copy would share the id of its
// source. It is not thread-safe; the owning thread makes structural changes at a synchronization point.
class World {
public:
    World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&& other) noexcept;
    World& operator=(World&& other) noexcept;

    [[nodiscard]] std::uint32_t id() const noexcept {
        return id_;
    }

    // Creates an entity with no persistent id.
    [[nodiscard]] EntityHandle create();

    // Creates an entity that carries a persistent id (for example a scene node GUID). The id is kept beside the
    // entity and never changes the index or generation the world assigns.
    [[nodiscard]] core::Result<EntityHandle, WorldError> create(const core::Guid& persistent_id);

    // Destroys a live entity and drops its persistent id. Returns false for a stale, foreign, or invalid handle.
    bool destroy(EntityHandle handle);

    // True only for a handle that this world issued and whose entity has not been destroyed since.
    [[nodiscard]] bool alive(EntityHandle handle) const noexcept;

    // Looks up the live entity that carries the persistent id, if any.
    [[nodiscard]] std::optional<EntityHandle> find(const core::Guid& persistent_id) const;

    // Returns the persistent id of a live entity, or nullopt when the handle is not live or has no id.
    [[nodiscard]] std::optional<core::Guid> persistent_id(EntityHandle handle) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return live_count_;
    }

private:
    struct Slot {
        std::uint32_t generation = 1;  // advanced on every destroy; 0 is skipped on wrap
        bool live = false;
        std::optional<core::Guid> persistent_id;
    };

    [[nodiscard]] EntityHandle make_handle(std::uint32_t index) const noexcept;
    [[nodiscard]] const Slot* find_slot(EntityHandle handle) const noexcept;

    std::uint32_t id_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<core::Guid, std::uint32_t> by_guid_;  // persistent id -> slot index
    std::size_t live_count_ = 0;
};

}  // namespace engine::ecs
