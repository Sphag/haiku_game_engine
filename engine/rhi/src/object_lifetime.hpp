#pragma once

// Object lifetime and queue timelines shared by the GPU backends (DX12 and Vulkan). An object that a submission names stays alive
// until that submission's timeline value completes, even after destroy() removed it from its handle table. Each backend keeps
// one timeline per queue type and reports completion through its own fence primitive.

#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace engine::rhi {

// One timeline per queue type.
inline constexpr std::size_t kQueueCount = 3;

[[nodiscard]] constexpr std::size_t queue_slot(QueueType queue) noexcept {
    switch (queue) {
        case QueueType::Graphics:
            return 0;
        case QueueType::Compute:
            return 1;
        case QueueType::Copy:
            return 2;
    }
    return 0;
}

// A point on one queue's timeline. The value is the timeline counter after the submission that produced it.
struct Stamp {
    QueueType queue = QueueType::Graphics;
    std::uint64_t value = 0;
};

// Base of every object a submission can name. `last_use` holds, per queue, the timeline value of the last submission that used
// the object. The object is kept alive until those values complete, even after destroy() removed it from its table.
struct ObjectBase : std::enable_shared_from_this<ObjectBase> {
    virtual ~ObjectBase() = default;
    bool destroyed = false;
    std::array<std::uint64_t, kQueueCount> last_use{};
};

// Holds the object for one handle. Generations change when a slot is freed, so a stale handle is rejected even after reuse.
template <typename Tag, typename Object>
class Table {
public:
    [[nodiscard]] Handle<Tag> add(std::shared_ptr<Object> object, std::uint32_t device_id) {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
            slots_[index].object = std::move(object);
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(Slot{std::move(object), 1});
        }
        ++live_;
        return Handle<Tag>{index, slots_[index].generation, device_id};
    }

    [[nodiscard]] std::size_t live() const noexcept {
        return live_;
    }

    // nullptr when the handle is stale, destroyed, from another device, or never existed.
    [[nodiscard]] Object* find(const Handle<Tag>& handle, std::uint32_t device_id) const {
        if (handle.device != device_id || handle.index >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[handle.index];
        if (!slot.object || slot.generation != handle.generation) {
            return nullptr;
        }
        return slot.object.get();
    }

    // Removes the object from the table and returns it. nullptr when the handle is not live.
    [[nodiscard]] std::shared_ptr<Object> take(const Handle<Tag>& handle, std::uint32_t device_id) {
        if (find(handle, device_id) == nullptr) {
            return nullptr;
        }
        Slot& slot = slots_[handle.index];
        std::shared_ptr<Object> object = std::move(slot.object);
        slot.object.reset();
        if (++slot.generation == 0) {
            slot.generation = 1;  // generation 0 is never live
        }
        free_.push_back(handle.index);
        --live_;
        return object;
    }

    // Releases every object at once. Only a device's teardown calls this, after its queues are idle.
    void clear() {
        slots_.clear();
        free_.clear();
        live_ = 0;
    }

private:
    struct Slot {
        std::shared_ptr<Object> object;
        std::uint32_t generation = 1;  // never 0
    };
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t live_ = 0;
};

// A submission's hold on objects, released when its timeline value completes.
struct Retained {
    Stamp stamp;
    std::vector<std::shared_ptr<void>> objects;
};

}  // namespace engine::rhi
