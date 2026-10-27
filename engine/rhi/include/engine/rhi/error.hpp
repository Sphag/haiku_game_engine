#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace engine::rhi {

enum class RhiErrorCode : std::uint8_t {
    InvalidHandle,     // handle is stale, destroyed, from another device, or never existed
    InvalidDescriptor, // a description or a binding is malformed or does not match its resource
    InvalidState,      // a command or submission is not allowed in the current recording or queue state
    StateMismatch,     // a transition's `before` state or a use-time state differs from the tracked state
    FeatureDisabled,   // the feature is unsupported or forced off on this device
    NotReady,          // a result (timestamp) exists only after the producing submission completes
    OutOfRange,        // an index, count, offset, or size is outside its resource
    Unsupported,       // this backend is not built, or this device has no such operation (see the message)
    DeviceLost,        // the GPU was removed or reset; every object on the device must be recreated
};

struct RhiError {
    RhiErrorCode code = RhiErrorCode::InvalidState;
    std::string message;  // names the call and the rule, for logs and test output
};

template <typename T>
using RhiResult = core::Result<T, RhiError>;
using RhiStatus = RhiResult<core::Unit>;

template <typename T = core::Unit>
[[nodiscard]] RhiResult<T> fail(RhiErrorCode code, std::string message) {
    return core::failure(RhiError{code, std::move(message)});
}

}  // namespace engine::rhi
