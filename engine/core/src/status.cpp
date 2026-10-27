#include "engine/core/status.hpp"

namespace engine::core {

std::string_view to_string(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Ok:
            return "Ok";
        case StatusCode::InvalidState:
            return "InvalidState";
    }
    return "Unknown";
}

}  // namespace engine::core
