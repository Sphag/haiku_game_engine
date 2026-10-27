#pragma once

// Input routing between editor/UI and gameplay (US-011). Each frame the platform's events pass through the router.
// Events from a device class that an editor or UI owner has captured go to captured_events(), and gameplay never sees
// them. Everything else reaches gameplay_events() and the gameplay InputState.
//
// Gameplay's view stays consistent with the devices. When a capture starts while a key is held, gameplay gets a
// release for it. When the capture ends while the key is still held, gameplay gets a press. Gamepad connection and
// axes follow the same rule. Entering Play releases all capture. Gameplay then sees every device as it is, and no
// capture request is accepted until Edit mode returns.
//
// The router is pure and builds on every OS. It consumes the events that Platform::drain_input_events() returns.

#include "engine/platform/input.hpp"
#include "engine/platform/input_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::platform {

// Device classes a capture can take. A capture request is a bit set of these.
inline constexpr std::uint8_t kCaptureKeyboard = 1u << 0;  // KeyPressed, KeyReleased, TextInput
inline constexpr std::uint8_t kCaptureMouse = 1u << 1;     // MouseMoved, mouse buttons, wheel
inline constexpr std::uint8_t kCaptureGamepad = 1u << 2;   // gamepad connection, buttons, axes
inline constexpr std::uint8_t kCaptureAll = kCaptureKeyboard | kCaptureMouse | kCaptureGamepad;

// Who requests capture. Each owner's request is kept apart, so one can release its own without touching the other's.
enum class CaptureOwner : std::uint8_t { Editor, Ui, Count };
inline constexpr std::size_t kCaptureOwnerCount = static_cast<std::size_t>(CaptureOwner::Count);

// Edit: owners may capture, and gameplay sees the rest. Play: gameplay owns every device, and requests are refused.
enum class RouterMode : std::uint8_t { Edit, Play };

class InputRouter {
public:
    // Sets the device classes `owner` captures, replacing its previous request. Returns false, and changes nothing,
    // in Play mode.
    bool set_capture(CaptureOwner owner, std::uint8_t classes);
    void release_capture(CaptureOwner owner);

    // The union of every owner's request.
    [[nodiscard]] std::uint8_t captured_classes() const noexcept;

    // Entering Play releases all capture. Leaving Play releases nothing, because capture was already released.
    void set_mode(RouterMode mode);
    [[nodiscard]] RouterMode mode() const noexcept;

    // Routes one frame of events, in order. Call once per frame, after the platform drained its events.
    void route(const std::vector<InputEvent>& frame);

    // Events gameplay receives this frame. Includes the synthetic releases and presses that keep its state consistent.
    [[nodiscard]] const std::vector<InputEvent>& gameplay_events() const noexcept;
    // Events of captured device classes this frame, for the editor or UI.
    [[nodiscard]] const std::vector<InputEvent>& captured_events() const noexcept;

    // The state gameplay reads: the same device input with captured classes held at rest.
    [[nodiscard]] const InputState& gameplay_input() const noexcept;
    // The physical device state, regardless of capture. Editor tools read this to show what is held.
    [[nodiscard]] const InputState& device_input() const noexcept;

private:
    void emit_to_gameplay(const InputEvent& event);
    void reconcile_with_devices(std::uint8_t captured);

    std::array<std::uint8_t, kCaptureOwnerCount> requests_{};
    RouterMode mode_ = RouterMode::Edit;
    InputState device_;
    InputState gameplay_;
    std::vector<InputEvent> gameplay_events_;
    std::vector<InputEvent> captured_events_;
    // Window of the latest keyboard or mouse event. Synthetic events carry it, as the focus window.
    WindowId focus_window_;
};

}  // namespace engine::platform
