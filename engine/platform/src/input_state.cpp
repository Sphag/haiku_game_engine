#include "engine/platform/input_state.hpp"

#include <variant>

namespace engine::platform {

namespace {

// Sets the held state for `index` and records the edge. Returns false when the state already matches.
template <std::size_t N>
bool press_edge(std::array<bool, N>& down, std::array<bool, N>& pressed, std::size_t index) noexcept {
    if (down[index]) {
        return false;
    }
    down[index] = true;
    pressed[index] = true;
    return true;
}

template <std::size_t N>
bool release_edge(std::array<bool, N>& down, std::array<bool, N>& released, std::size_t index) noexcept {
    if (!down[index]) {
        return false;
    }
    down[index] = false;
    released[index] = true;
    return true;
}

}  // namespace

void InputState::begin_frame() noexcept {
    keys_pressed_.fill(false);
    keys_released_.fill(false);
    mouse_pressed_.fill(false);
    mouse_released_.fill(false);
    mouse_delta_ = {};
    wheel_x_ = 0.0;
    wheel_y_ = 0.0;
    text_.clear();
    for (Gamepad& pad : gamepads_) {
        pad.pressed.fill(false);
        pad.released.fill(false);
    }
    duplicates_ = 0;
}

bool InputState::apply(const InputEvent& event) {
    // Redundant transitions are counted. Invalid input (Unknown key, slot out of range) is rejected without a count.
    if (const auto* key = std::get_if<KeyPressed>(&event.data)) {
        if (key->key == Key::Unknown) {
            return false;
        }
        const bool changed = press_edge(keys_down_, keys_pressed_, static_cast<std::size_t>(key->key));
        duplicates_ += changed ? 0 : 1;
        return changed;
    }
    if (const auto* key = std::get_if<KeyReleased>(&event.data)) {
        if (key->key == Key::Unknown) {
            return false;
        }
        const bool changed = release_edge(keys_down_, keys_released_, static_cast<std::size_t>(key->key));
        duplicates_ += changed ? 0 : 1;
        return changed;
    }
    if (const auto* text = std::get_if<TextInput>(&event.data)) {
        if (text->utf8.empty()) {
            return false;
        }
        text_ += text->utf8;
        return true;
    }
    if (const auto* motion = std::get_if<MouseMoved>(&event.data)) {
        if (motion->dx == 0 && motion->dy == 0) {
            return false;
        }
        mouse_delta_.dx += motion->dx;
        mouse_delta_.dy += motion->dy;
        return true;
    }
    if (const auto* press = std::get_if<MouseButtonPressed>(&event.data)) {
        const bool changed = press_edge(mouse_down_, mouse_pressed_, static_cast<std::size_t>(press->button));
        duplicates_ += changed ? 0 : 1;
        return changed;
    }
    if (const auto* release = std::get_if<MouseButtonReleased>(&event.data)) {
        const bool changed = release_edge(mouse_down_, mouse_released_, static_cast<std::size_t>(release->button));
        duplicates_ += changed ? 0 : 1;
        return changed;
    }
    if (const auto* wheel = std::get_if<MouseWheelScrolled>(&event.data)) {
        if (wheel->notches_x == 0.0 && wheel->notches_y == 0.0) {
            return false;
        }
        wheel_x_ += wheel->notches_x;
        wheel_y_ += wheel->notches_y;
        return true;
    }
    if (const auto* connected = std::get_if<GamepadConnected>(&event.data)) {
        if (connected->slot >= kMaxGamepads) {
            return false;
        }
        Gamepad& pad = gamepads_[connected->slot];
        if (pad.connected) {
            ++duplicates_;
            return false;
        }
        pad.connected = true;
        return true;
    }
    if (const auto* disconnected = std::get_if<GamepadDisconnected>(&event.data)) {
        if (disconnected->slot >= kMaxGamepads) {
            return false;
        }
        Gamepad& pad = gamepads_[disconnected->slot];
        if (!pad.connected) {
            ++duplicates_;
            return false;
        }
        // diff_gamepad releases the buttons and resets the axes before it sends this event, so both are already at
        // rest. Clearing them again keeps a stray disconnect from leaving a button held. The edges stay, so this
        // frame's releases remain visible.
        pad.connected = false;
        pad.down.fill(false);
        pad.axes.fill(0.0f);
        return true;
    }
    if (const auto* press = std::get_if<GamepadButtonPressed>(&event.data)) {
        if (press->slot >= kMaxGamepads) {
            return false;
        }
        Gamepad& pad = gamepads_[press->slot];
        const bool changed = press_edge(pad.down, pad.pressed, static_cast<std::size_t>(press->button));
        duplicates_ += changed ? 0 : 1;
        return changed;
    }
    if (const auto* release = std::get_if<GamepadButtonReleased>(&event.data)) {
        if (release->slot >= kMaxGamepads) {
            return false;
        }
        Gamepad& pad = gamepads_[release->slot];
        const bool changed = release_edge(pad.down, pad.released, static_cast<std::size_t>(release->button));
        duplicates_ += changed ? 0 : 1;
        return changed;
    }
    if (const auto* axis = std::get_if<GamepadAxisChanged>(&event.data)) {
        if (axis->slot >= kMaxGamepads) {
            return false;
        }
        float& value = gamepads_[axis->slot].axes[static_cast<std::size_t>(axis->axis)];
        if (value == axis->value) {
            ++duplicates_;
            return false;
        }
        value = axis->value;
        return true;
    }
    return false;
}

std::vector<InputEvent> InputState::release_held(WindowId window) const {
    std::vector<InputEvent> events;
    for (std::size_t index = 0; index < kKeyCount; ++index) {
        if (keys_down_[index]) {
            events.push_back(InputEvent{window, KeyReleased{static_cast<Key>(index)}});
        }
    }
    for (std::size_t index = 0; index < kMouseButtonCount; ++index) {
        if (mouse_down_[index]) {
            events.push_back(InputEvent{window, MouseButtonReleased{static_cast<MouseButton>(index)}});
        }
    }
    return events;
}

bool InputState::key_down(Key key) const noexcept {
    return keys_down_[static_cast<std::size_t>(key)];
}

bool InputState::key_pressed(Key key) const noexcept {
    return keys_pressed_[static_cast<std::size_t>(key)];
}

bool InputState::key_released(Key key) const noexcept {
    return keys_released_[static_cast<std::size_t>(key)];
}

bool InputState::mouse_down(MouseButton button) const noexcept {
    return mouse_down_[static_cast<std::size_t>(button)];
}

bool InputState::mouse_pressed(MouseButton button) const noexcept {
    return mouse_pressed_[static_cast<std::size_t>(button)];
}

bool InputState::mouse_released(MouseButton button) const noexcept {
    return mouse_released_[static_cast<std::size_t>(button)];
}

InputState::MouseDelta InputState::mouse_delta() const noexcept {
    return mouse_delta_;
}

double InputState::wheel_x() const noexcept {
    return wheel_x_;
}

double InputState::wheel_y() const noexcept {
    return wheel_y_;
}

const std::string& InputState::text() const noexcept {
    return text_;
}

bool InputState::gamepad_connected(std::uint32_t slot) const noexcept {
    return slot < kMaxGamepads && gamepads_[slot].connected;
}

bool InputState::gamepad_down(std::uint32_t slot, GamepadButton button) const noexcept {
    return slot < kMaxGamepads && gamepads_[slot].down[static_cast<std::size_t>(button)];
}

bool InputState::gamepad_pressed(std::uint32_t slot, GamepadButton button) const noexcept {
    return slot < kMaxGamepads && gamepads_[slot].pressed[static_cast<std::size_t>(button)];
}

bool InputState::gamepad_released(std::uint32_t slot, GamepadButton button) const noexcept {
    return slot < kMaxGamepads && gamepads_[slot].released[static_cast<std::size_t>(button)];
}

float InputState::gamepad_axis(std::uint32_t slot, GamepadAxis axis) const noexcept {
    return slot < kMaxGamepads ? gamepads_[slot].axes[static_cast<std::size_t>(axis)] : 0.0f;
}

std::size_t InputState::duplicates_ignored() const noexcept {
    return duplicates_;
}

}  // namespace engine::platform
