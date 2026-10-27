#pragma once

#include "engine/core/status.hpp"

#include <string_view>

namespace engine::runtime {

enum class LifecycleState {
    Created,
    Running,
    Stopped,
};

[[nodiscard]] std::string_view to_string(LifecycleState state) noexcept;

// Single-use runtime lifecycle: Created -> Running -> Stopped.
// Out-of-order calls return InvalidState and leave the state unchanged.
class Lifecycle {
public:
    [[nodiscard]] core::Status start() noexcept;
    [[nodiscard]] core::Status stop() noexcept;

    [[nodiscard]] LifecycleState state() const noexcept {
        return state_;
    }

private:
    LifecycleState state_ = LifecycleState::Created;
};

}  // namespace engine::runtime
