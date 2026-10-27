#include "engine/ecs/world.hpp"

#include <atomic>
#include <utility>

namespace engine::ecs {
namespace {

// Process-wide, so two worlds never share an id even when they live on different threads. Id 0 is never issued.
std::uint32_t next_world_id() {
    static std::atomic<std::uint32_t> counter{0};
    return ++counter;
}

std::uint32_t next_generation(std::uint32_t generation) noexcept {
    // Generation 0 means "never valid", so the counter skips it when it wraps.
    const std::uint32_t next = generation + 1;
    return next == 0 ? 1 : next;
}

}  // namespace

World::World() : id_(next_world_id()) {}

World::World(World&& other) noexcept
    : id_(std::exchange(other.id_, 0)),
      slots_(std::move(other.slots_)),
      free_(std::move(other.free_)),
      by_guid_(std::move(other.by_guid_)),
      live_count_(std::exchange(other.live_count_, 0)) {
    other.slots_.clear();
    other.free_.clear();
    other.by_guid_.clear();
}

World& World::operator=(World&& other) noexcept {
    if (this != &other) {
        id_ = std::exchange(other.id_, 0);
        slots_ = std::move(other.slots_);
        free_ = std::move(other.free_);
        by_guid_ = std::move(other.by_guid_);
        live_count_ = std::exchange(other.live_count_, 0);
        other.slots_.clear();
        other.free_.clear();
        other.by_guid_.clear();
    }
    return *this;
}

EntityHandle World::create() {
    std::uint32_t index = 0;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(Slot{});
    }
    slots_[index].live = true;
    ++live_count_;
    return make_handle(index);
}

core::Result<EntityHandle, WorldError> World::create(const core::Guid& persistent_id) {
    if (persistent_id.is_nil()) {
        return core::failure(WorldError::NilGuid);
    }
    if (by_guid_.contains(persistent_id)) {
        return core::failure(WorldError::DuplicateGuid);
    }
    const EntityHandle handle = create();
    slots_[handle.index].persistent_id = persistent_id;
    by_guid_.emplace(persistent_id, handle.index);
    return core::success(handle);
}

bool World::destroy(EntityHandle handle) {
    if (find_slot(handle) == nullptr) {
        return false;
    }
    Slot& slot = slots_[handle.index];
    if (slot.persistent_id.has_value()) {
        by_guid_.erase(*slot.persistent_id);
        slot.persistent_id.reset();
    }
    slot.live = false;
    slot.generation = next_generation(slot.generation);
    free_.push_back(handle.index);
    --live_count_;
    return true;
}

bool World::alive(EntityHandle handle) const noexcept {
    return find_slot(handle) != nullptr;
}

std::optional<EntityHandle> World::find(const core::Guid& persistent_id) const {
    const auto found = by_guid_.find(persistent_id);
    if (found == by_guid_.end()) {
        return std::nullopt;
    }
    return make_handle(found->second);
}

std::optional<core::Guid> World::persistent_id(EntityHandle handle) const {
    const Slot* slot = find_slot(handle);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->persistent_id;
}

EntityHandle World::make_handle(std::uint32_t index) const noexcept {
    return EntityHandle{index, slots_[index].generation, id_};
}

const World::Slot* World::find_slot(EntityHandle handle) const noexcept {
    if (!handle.valid() || handle.world != id_ || handle.index >= slots_.size()) {
        return nullptr;
    }
    const Slot& slot = slots_[handle.index];
    return slot.live && slot.generation == handle.generation ? &slot : nullptr;
}

}  // namespace engine::ecs
