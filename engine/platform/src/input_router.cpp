#include "engine/platform/input_router.hpp"

#include <variant>

namespace engine::platform {
namespace {

// The capture class an event belongs to.
struct DeviceClassOf {
    std::uint8_t operator()(const KeyPressed&) const noexcept { return kCaptureKeyboard; }
    std::uint8_t operator()(const KeyReleased&) const noexcept { return kCaptureKeyboard; }
    std::uint8_t operator()(const TextInput&) const noexcept { return kCaptureKeyboard; }
    std::uint8_t operator()(const MouseMoved&) const noexcept { return kCaptureMouse; }
    std::uint8_t operator()(const MouseButtonPressed&) const noexcept { return kCaptureMouse; }
    std::uint8_t operator()(const MouseButtonReleased&) const noexcept { return kCaptureMouse; }
    std::uint8_t operator()(const MouseWheelScrolled&) const noexcept { return kCaptureMouse; }
    std::uint8_t operator()(const GamepadConnected&) const noexcept { return kCaptureGamepad; }
    std::uint8_t operator()(const GamepadDisconnected&) const noexcept { return kCaptureGamepad; }
    std::uint8_t operator()(const GamepadButtonPressed&) const noexcept { return kCaptureGamepad; }
    std::uint8_t operator()(const GamepadButtonReleased&) const noexcept { return kCaptureGamepad; }
    std::uint8_t operator()(const GamepadAxisChanged&) const noexcept { return kCaptureGamepad; }
};

}  // namespace

bool InputRouter::set_capture(CaptureOwner owner, std::uint8_t classes) {
    if (mode_ == RouterMode::Play) {
        return false;
    }
    requests_[static_cast<std::size_t>(owner)] = static_cast<std::uint8_t>(classes & kCaptureAll);
    return true;
}

void InputRouter::release_capture(CaptureOwner owner) {
    requests_[static_cast<std::size_t>(owner)] = 0;
}

std::uint8_t InputRouter::captured_classes() const noexcept {
    std::uint8_t captured = 0;
    for (const std::uint8_t request : requests_) {
        captured = static_cast<std::uint8_t>(captured | request);
    }
    return captured;
}

void InputRouter::set_mode(RouterMode mode) {
    mode_ = mode;
    if (mode == RouterMode::Play) {
        requests_.fill(0);
    }
}

RouterMode InputRouter::mode() const noexcept {
    return mode_;
}

void InputRouter::route(const std::vector<InputEvent>& frame) {
    device_.begin_frame();
    gameplay_.begin_frame();
    gameplay_events_.clear();
    captured_events_.clear();

    const std::uint8_t captured = captured_classes();
    for (const InputEvent& event : frame) {
        device_.apply(event);
        const std::uint8_t device_class = std::visit(DeviceClassOf{}, event.data);
        if (device_class != kCaptureGamepad && event.window.valid()) {
            focus_window_ = event.window;
        }
        if ((device_class & captured) != 0) {
            captured_events_.push_back(event);
        } else {
            emit_to_gameplay(event);
        }
    }
    reconcile_with_devices(captured);
}

const std::vector<InputEvent>& InputRouter::gameplay_events() const noexcept {
    return gameplay_events_;
}

const std::vector<InputEvent>& InputRouter::captured_events() const noexcept {
    return captured_events_;
}

const InputState& InputRouter::gameplay_input() const noexcept {
    return gameplay_;
}

const InputState& InputRouter::device_input() const noexcept {
    return device_;
}

void InputRouter::emit_to_gameplay(const InputEvent& event) {
    gameplay_events_.push_back(event);
    gameplay_.apply(event);
}

// Brings gameplay's state in line with the devices, except that captured classes read as at rest. Each synthetic event
// is applied as it is emitted, so the state is compared with the devices after every change.
void InputRouter::reconcile_with_devices(std::uint8_t captured) {
    const bool keyboard_captured = (captured & kCaptureKeyboard) != 0;
    const bool mouse_captured = (captured & kCaptureMouse) != 0;
    const bool gamepad_captured = (captured & kCaptureGamepad) != 0;

    // Connection comes first, so button and axis changes below apply to a pad that gameplay can see.
    for (std::uint32_t slot = 0; slot < kMaxGamepads; ++slot) {
        const bool wanted = device_.gamepad_connected(slot) && !gamepad_captured;
        if (wanted == gameplay_.gamepad_connected(slot)) {
            continue;
        }
        if (wanted) {
            emit_to_gameplay(InputEvent{focus_window_, GamepadConnected{slot}});
            continue;
        }
        // A disconnect clears the pad's held buttons without edges. Release them first, so gameplay sees the releases.
        for (std::size_t button = 0; button < kGamepadButtonCount; ++button) {
            const auto pad_button = static_cast<GamepadButton>(button);
            if (gameplay_.gamepad_down(slot, pad_button)) {
                emit_to_gameplay(InputEvent{focus_window_, GamepadButtonReleased{slot, pad_button}});
            }
        }
        emit_to_gameplay(InputEvent{focus_window_, GamepadDisconnected{slot}});
    }

    for (std::size_t index = 1; index < kKeyCount; ++index) {
        const auto key = static_cast<Key>(index);
        const bool wanted = !keyboard_captured && device_.key_down(key);
        if (wanted != gameplay_.key_down(key)) {
            if (wanted) {
                emit_to_gameplay(InputEvent{focus_window_, KeyPressed{key}});
            } else {
                emit_to_gameplay(InputEvent{focus_window_, KeyReleased{key}});
            }
        }
    }

    for (std::size_t index = 0; index < kMouseButtonCount; ++index) {
        const auto button = static_cast<MouseButton>(index);
        const bool wanted = !mouse_captured && device_.mouse_down(button);
        if (wanted != gameplay_.mouse_down(button)) {
            if (wanted) {
                emit_to_gameplay(InputEvent{focus_window_, MouseButtonPressed{button}});
            } else {
                emit_to_gameplay(InputEvent{focus_window_, MouseButtonReleased{button}});
            }
        }
    }

    for (std::uint32_t slot = 0; slot < kMaxGamepads; ++slot) {
        for (std::size_t index = 0; index < kGamepadButtonCount; ++index) {
            const auto button = static_cast<GamepadButton>(index);
            const bool wanted = !gamepad_captured && device_.gamepad_down(slot, button);
            if (wanted != gameplay_.gamepad_down(slot, button)) {
                if (wanted) {
                    emit_to_gameplay(InputEvent{focus_window_, GamepadButtonPressed{slot, button}});
                } else {
                    emit_to_gameplay(InputEvent{focus_window_, GamepadButtonReleased{slot, button}});
                }
            }
        }
        for (std::size_t index = 0; index < kGamepadAxisCount; ++index) {
            const auto axis = static_cast<GamepadAxis>(index);
            const float wanted = gamepad_captured ? 0.0f : device_.gamepad_axis(slot, axis);
            if (wanted != gameplay_.gamepad_axis(slot, axis)) {
                emit_to_gameplay(InputEvent{focus_window_, GamepadAxisChanged{slot, axis, wanted}});
            }
        }
    }
}

}  // namespace engine::platform
