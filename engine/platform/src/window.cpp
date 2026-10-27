#include "engine/platform/window.hpp"

#include "engine/core/utf8.hpp"

#include <cmath>

namespace engine::platform {

std::string_view to_string(PlatformErrorCode code) noexcept {
    switch (code) {
        case PlatformErrorCode::Unsupported:
            return "Unsupported";
        case PlatformErrorCode::InvalidArgument:
            return "InvalidArgument";
        case PlatformErrorCode::CreateFailed:
            return "CreateFailed";
        case PlatformErrorCode::NotFound:
            return "NotFound";
        case PlatformErrorCode::NoDisplay:
            return "NoDisplay";
    }
    return "Unknown";
}

core::Result<core::Unit, PlatformError> validate_window_desc(const WindowDesc& desc) {
    if (!core::is_valid_utf8(desc.title)) {
        return core::failure(PlatformError{PlatformErrorCode::InvalidArgument, "window title is not valid UTF-8"});
    }
    if (desc.title.find('\0') != std::string::npos) {
        return core::failure(PlatformError{PlatformErrorCode::InvalidArgument, "window title contains a NUL byte"});
    }

    const auto check_dimension = [](double value, const char* name) -> core::Result<core::Unit, PlatformError> {
        if (!std::isfinite(value) || value < 1.0 || value > kMaxLogicalDimension) {
            return core::failure(PlatformError{
                PlatformErrorCode::InvalidArgument,
                std::string(name) + " must be between 1 and " + std::to_string(static_cast<int>(kMaxLogicalDimension)) +
                    " device-independent pixels"});
        }
        return core::success();
    };

    if (auto width = check_dimension(desc.size.width, "window width"); !width.ok()) {
        return width;
    }
    return check_dimension(desc.size.height, "window height");
}

}  // namespace engine::platform
