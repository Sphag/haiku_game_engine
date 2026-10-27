// Windowing for operating systems without an in-house backend (Windows uses Win32, Linux uses X11).
// create() reports Unsupported, so no Platform object exists and the members below are never reached.

#include "engine/core/assert.hpp"
#include "engine/platform/platform.hpp"

namespace engine::platform {

struct Platform::Impl {};

core::Result<std::unique_ptr<Platform>, PlatformError> Platform::create() {
    return core::failure(PlatformError{
        PlatformErrorCode::Unsupported,
        "no in-house windowing backend exists for this operating system yet (the X11 backend is US-008)"});
}

Platform::Platform(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Platform::~Platform() = default;

core::Result<WindowId, PlatformError> Platform::create_window(const WindowDesc&) {
    ENGINE_ASSERT(false, "create_window called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<core::Unit, PlatformError> Platform::destroy_window(WindowId) {
    ENGINE_ASSERT(false, "destroy_window called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<core::Unit, PlatformError> Platform::minimize_window(WindowId) {
    ENGINE_ASSERT(false, "minimize_window called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<core::Unit, PlatformError> Platform::restore_window(WindowId) {
    ENGINE_ASSERT(false, "restore_window called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<WindowInfo, PlatformError> Platform::window_info(WindowId) const {
    ENGINE_ASSERT(false, "window_info called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<Win32WindowHandles, PlatformError> Platform::win32_handles(WindowId) const {
    ENGINE_ASSERT(false, "win32_handles called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<XlibWindowHandles, PlatformError> Platform::xlib_handles(WindowId) const {
    ENGINE_ASSERT(false, "xlib_handles called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

void Platform::pump_events() {
    ENGINE_ASSERT(false, "pump_events called on a Platform that could not be created");
}

std::vector<WindowEvent> Platform::drain_events() {
    ENGINE_ASSERT(false, "drain_events called on a Platform that could not be created");
    return {};
}

void Platform::set_modal_tick(ModalTick) {
    ENGINE_ASSERT(false, "set_modal_tick called on a Platform that could not be created");
}

std::size_t Platform::window_count() const noexcept {
    ENGINE_ASSERT(false, "window_count called on a Platform that could not be created");
    return 0;
}

std::vector<InputEvent> Platform::drain_input_events() {
    ENGINE_ASSERT(false, "drain_input_events called on a Platform that could not be created");
    return {};
}

core::Result<core::Unit, PlatformError> Platform::set_cursor_capture(WindowId, bool) {
    ENGINE_ASSERT(false, "set_cursor_capture called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<bool, PlatformError> Platform::cursor_captured(WindowId) const {
    ENGINE_ASSERT(false, "cursor_captured called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

core::Result<PointerPosition, PlatformError> Platform::pointer_position(WindowId) const {
    ENGINE_ASSERT(false, "pointer_position called on a Platform that could not be created");
    return core::failure(PlatformError{PlatformErrorCode::Unsupported, "unreachable"});
}

}  // namespace engine::platform

