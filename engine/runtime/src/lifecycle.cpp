#include "engine/runtime/lifecycle.hpp"

namespace engine::runtime {

std::string_view to_string(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Created:
            return "Created";
        case LifecycleState::Running:
            return "Running";
        case LifecycleState::Stopped:
            return "Stopped";
    }
    return "Unknown";
}

core::Status Lifecycle::start() noexcept {
    if (state_ != LifecycleState::Created) {
        return {core::StatusCode::InvalidState};
    }
    state_ = LifecycleState::Running;
    return {};
}

core::Status Lifecycle::stop() noexcept {
    if (state_ != LifecycleState::Running) {
        return {core::StatusCode::InvalidState};
    }
    state_ = LifecycleState::Stopped;
    return {};
}

}  // namespace engine::runtime
