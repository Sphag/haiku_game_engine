#pragma once

#include <cstdint>

namespace engine::ecs {

// Reference to an entity inside one World. The index selects a slot, and the generation changes every time that slot
// is destroyed, so a handle kept after its entity was destroyed never resolves to a later entity in the same slot.
// The world id ties the handle to its owner: a handle from another World is rejected even when index and generation
// happen to match.
struct EntityHandle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;  // 0 is never a live generation
    std::uint32_t world = 0;       // 0 is never a live world id

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation != 0 && world != 0;
    }

    friend constexpr bool operator==(const EntityHandle& lhs, const EntityHandle& rhs) noexcept {
        return lhs.index == rhs.index && lhs.generation == rhs.generation && lhs.world == rhs.world;
    }
    friend constexpr bool operator!=(const EntityHandle& lhs, const EntityHandle& rhs) noexcept {
        return !(lhs == rhs);
    }
};

}  // namespace engine::ecs
