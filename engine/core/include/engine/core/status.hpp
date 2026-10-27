#pragma once

#include <string_view>

namespace engine::core {

enum class StatusCode {
    Ok,
    InvalidState,
};

[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

// Result of an engine operation that can fail without throwing.
struct Status {
    StatusCode code = StatusCode::Ok;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return code == StatusCode::Ok;
    }
};

}  // namespace engine::core
