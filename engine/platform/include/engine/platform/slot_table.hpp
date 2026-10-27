#pragma once

#include "engine/platform/window.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::platform {

// Stores values under WindowId keys. Erasing a value bumps its slot's generation, so every id issued for that slot
// stops resolving. Slots are reused, but their ids are not, and values keep their addresses only until the table
// grows; callers must not keep pointers across insert().
template <typename T>
class SlotTable {
public:
    [[nodiscard]] WindowId insert(T value) {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(Slot{});
        }
        Slot& slot = slots_[index];
        slot.value.emplace(std::move(value));
        ++count_;
        return WindowId{index, slot.generation};
    }

    [[nodiscard]] T* find(WindowId id) noexcept {
        Slot* slot = find_slot(id);
        return slot == nullptr ? nullptr : &*slot->value;
    }

    [[nodiscard]] const T* find(WindowId id) const noexcept {
        const Slot* slot = find_slot(id);
        return slot == nullptr ? nullptr : &*slot->value;
    }

    // Returns false when the id is not live.
    bool erase(WindowId id) {
        Slot* slot = find_slot(id);
        if (slot == nullptr) {
            return false;
        }
        slot->value.reset();
        slot->generation = next_generation(slot->generation);
        free_.push_back(id.index);
        --count_;
        return true;
    }

    // Calls fn(WindowId, T&) for each live value. fn must not insert or erase.
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (std::uint32_t index = 0; index < slots_.size(); ++index) {
            Slot& slot = slots_[index];
            if (slot.value.has_value()) {
                fn(WindowId{index, slot.generation}, *slot.value);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 1;
    };

    [[nodiscard]] Slot* find_slot(WindowId id) noexcept {
        if (!id.valid() || id.index >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[id.index];
        return slot.value.has_value() && slot.generation == id.generation ? &slot : nullptr;
    }

    [[nodiscard]] const Slot* find_slot(WindowId id) const noexcept {
        if (!id.valid() || id.index >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[id.index];
        return slot.value.has_value() && slot.generation == id.generation ? &slot : nullptr;
    }

    [[nodiscard]] static std::uint32_t next_generation(std::uint32_t generation) noexcept {
        // Generation 0 means "never valid", so the counter skips it when it wraps.
        const std::uint32_t next = generation + 1;
        return next == 0 ? 1 : next;
    }

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t count_ = 0;
};

}  // namespace engine::platform
