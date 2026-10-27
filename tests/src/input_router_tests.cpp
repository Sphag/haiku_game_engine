// Input routing between editor/UI capture and gameplay (US-011). CPU-only: synthetic events go through InputRouter on
// every operating system. The checks cover what gameplay sees, the physical state, and the Play switch.

#include "engine/platform/input.hpp"
#include "engine/platform/input_router.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

using engine::platform::CaptureOwner;
using engine::platform::GamepadAxis;
using engine::platform::GamepadAxisChanged;
using engine::platform::GamepadButton;
using engine::platform::GamepadButtonPressed;
using engine::platform::GamepadButtonReleased;
using engine::platform::GamepadConnected;
using engine::platform::GamepadDisconnected;
using engine::platform::InputEvent;
using engine::platform::InputRouter;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::KeyPressed;
using engine::platform::KeyReleased;
using engine::platform::kCaptureAll;
using engine::platform::kCaptureGamepad;
using engine::platform::kCaptureKeyboard;
using engine::platform::kCaptureMouse;
using engine::platform::MouseButton;
using engine::platform::MouseButtonPressed;
using engine::platform::MouseButtonReleased;
using engine::platform::MouseMoved;
using engine::platform::RouterMode;
using engine::platform::TextInput;
using engine::platform::WindowId;

namespace {

constexpr WindowId kWindow{1, 1};

InputEvent window_event(engine::platform::InputEventData data) {
    return InputEvent{kWindow, std::move(data)};
}
InputEvent pad_event(engine::platform::InputEventData data) {
    return InputEvent{WindowId{}, std::move(data)};
}

// True when `events` holds an event whose data is alternative T and matches `matches`.
template <typename T, typename Matches>
bool has_event(const std::vector<InputEvent>& events, Matches matches) {
    for (const InputEvent& event : events) {
        if (const auto* data = std::get_if<T>(&event.data)) {
            if (matches(*data)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Routing: captured keyboard input never reaches gameplay", "[unit]") {
    InputRouter router;
    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));

    router.route({window_event(KeyPressed{Key::W}), window_event(TextInput{"w"}), window_event(KeyReleased{Key::W})});

    CHECK(router.gameplay_events().empty());
    CHECK(router.captured_events().size() == 3);
    CHECK_FALSE(router.gameplay_input().key_down(Key::W));
    CHECK_FALSE(router.gameplay_input().key_pressed(Key::W));
    CHECK(router.gameplay_input().text().empty());
}

TEST_CASE("Routing: uncaptured input reaches gameplay in order", "[unit]") {
    InputRouter router;
    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureMouse));

    router.route({window_event(KeyPressed{Key::Space}), window_event(MouseMoved{3, 4})});

    CHECK(router.gameplay_events().size() == 1);
    CHECK(router.captured_events().size() == 1);
    CHECK(router.gameplay_input().key_pressed(Key::Space));
    CHECK(router.gameplay_input().mouse_delta().dx == 0);
}

TEST_CASE("Routing: a capture that starts while a key is held releases it for gameplay", "[unit]") {
    InputRouter router;
    router.route({window_event(KeyPressed{Key::W})});
    REQUIRE(router.gameplay_input().key_down(Key::W));

    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));
    router.route({});

    CHECK(has_event<KeyReleased>(router.gameplay_events(), [](const KeyReleased& e) { return e.key == Key::W; }));
    CHECK_FALSE(router.gameplay_input().key_down(Key::W));
    CHECK(router.gameplay_input().key_released(Key::W));

    // The key's release, which the editor now receives, adds nothing to gameplay.
    router.route({window_event(KeyReleased{Key::W})});
    CHECK(router.gameplay_events().empty());
    CHECK(router.captured_events().size() == 1);
    CHECK_FALSE(router.device_input().key_down(Key::W));
}

TEST_CASE("Routing: ending a capture while a key is held presses it for gameplay", "[unit]") {
    InputRouter router;
    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));
    router.route({window_event(KeyPressed{Key::Space})});
    CHECK_FALSE(router.gameplay_input().key_down(Key::Space));

    router.release_capture(CaptureOwner::Editor);
    router.route({});

    CHECK(has_event<KeyPressed>(router.gameplay_events(), [](const KeyPressed& e) { return e.key == Key::Space; }));
    CHECK(router.gameplay_input().key_pressed(Key::Space));
    CHECK(router.gameplay_input().key_down(Key::Space));

    // Steady state: nothing more is sent while the key stays down.
    router.route({});
    CHECK(router.gameplay_events().empty());
    CHECK(router.gameplay_input().key_down(Key::Space));
}

TEST_CASE("Routing: a mouse capture stops motion, buttons, and wheel only", "[unit]") {
    InputRouter router;
    router.route({window_event(MouseButtonPressed{MouseButton::Left})});
    REQUIRE(router.gameplay_input().mouse_down(MouseButton::Left));

    REQUIRE(router.set_capture(CaptureOwner::Ui, kCaptureMouse));
    router.route({window_event(MouseMoved{10, -2}), window_event(MouseButtonReleased{MouseButton::Left})});

    CHECK(router.captured_events().size() == 2);
    // The button was held when the capture began, so gameplay gets its release. The captured release itself does not
    // reach gameplay a second time.
    REQUIRE(router.gameplay_events().size() == 1);
    CHECK(has_event<MouseButtonReleased>(router.gameplay_events(),
                                         [](const MouseButtonReleased& e) { return e.button == MouseButton::Left; }));
    CHECK_FALSE(router.gameplay_input().mouse_down(MouseButton::Left));
    CHECK(router.gameplay_input().mouse_delta().dx == 0);
    CHECK_FALSE(router.device_input().mouse_down(MouseButton::Left));
}

TEST_CASE("Routing: a gamepad capture reads as disconnected with axes at rest, then restores", "[unit]") {
    InputRouter router;
    router.route({pad_event(GamepadConnected{0}), pad_event(GamepadButtonPressed{0, GamepadButton::A}),
                  pad_event(GamepadAxisChanged{0, GamepadAxis::LeftX, 0.8f})});
    REQUIRE(router.gameplay_input().gamepad_connected(0));
    REQUIRE(router.gameplay_input().gamepad_down(0, GamepadButton::A));

    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureGamepad));
    router.route({});

    // The held button is released before the pad disconnects, so gameplay sees both edges.
    const auto& events = router.gameplay_events();
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<GamepadButtonReleased>(events[0].data));
    CHECK(std::holds_alternative<GamepadDisconnected>(events[1].data));
    CHECK_FALSE(router.gameplay_input().gamepad_connected(0));
    CHECK(router.gameplay_input().gamepad_axis(0, GamepadAxis::LeftX) == 0.0f);
    CHECK(router.device_input().gamepad_connected(0));

    router.release_capture(CaptureOwner::Editor);
    router.route({});

    CHECK(router.gameplay_input().gamepad_connected(0));
    CHECK(router.gameplay_input().gamepad_pressed(0, GamepadButton::A));
    CHECK(router.gameplay_input().gamepad_axis(0, GamepadAxis::LeftX) == 0.8f);
}

TEST_CASE("Routing: a captured pad is not reported as disconnected when it was never seen", "[unit]") {
    InputRouter router;
    REQUIRE(router.set_capture(CaptureOwner::Ui, kCaptureGamepad));
    router.route({pad_event(GamepadConnected{1})});

    CHECK(router.gameplay_events().empty());
    CHECK_FALSE(router.gameplay_input().gamepad_connected(1));
    CHECK(router.device_input().gamepad_connected(1));
}

TEST_CASE("Routing: each owner's request is separate and the union is captured", "[unit]") {
    InputRouter router;
    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));
    REQUIRE(router.set_capture(CaptureOwner::Ui, kCaptureMouse));
    CHECK(router.captured_classes() == (kCaptureKeyboard | kCaptureMouse));

    router.release_capture(CaptureOwner::Editor);
    CHECK(router.captured_classes() == kCaptureMouse);

    // Replacing a request drops the classes it no longer names, and bits outside the known classes are ignored.
    REQUIRE(router.set_capture(CaptureOwner::Ui, 0xFF));
    CHECK(router.captured_classes() == kCaptureAll);
    REQUIRE(router.set_capture(CaptureOwner::Ui, 0));
    CHECK(router.captured_classes() == 0);
}

TEST_CASE("Routing: entering Play releases every capture and refuses new ones", "[unit]") {
    InputRouter router;
    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureAll));
    router.route({window_event(KeyPressed{Key::Space})});
    REQUIRE(router.gameplay_input().key_down(Key::Space) == false);

    router.set_mode(RouterMode::Play);
    CHECK(router.mode() == RouterMode::Play);
    CHECK(router.captured_classes() == 0);
    CHECK_FALSE(router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));
    CHECK(router.captured_classes() == 0);

    // The key the editor held is now visible to gameplay, pressed on the first frame of Play.
    router.route({});
    CHECK(router.gameplay_input().key_pressed(Key::Space));
    CHECK(router.gameplay_input().key_down(Key::Space));

    // Back in Edit mode, capture can be requested again.
    router.set_mode(RouterMode::Edit);
    CHECK(router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));
    // Gameplay holds Space from the Play switch. Space is still down on the devices, but the editor now captures the
    // keyboard again, so gameplay gets a release for it.
    router.route({});
    REQUIRE(router.gameplay_events().size() == 1);
    CHECK(std::holds_alternative<KeyReleased>(router.gameplay_events()[0].data));
    CHECK_FALSE(router.gameplay_input().key_down(Key::Space));
}

TEST_CASE("Routing: the physical device state is kept even while captured", "[unit]") {
    InputRouter router;
    REQUIRE(router.set_capture(CaptureOwner::Editor, kCaptureAll));
    router.route({window_event(KeyPressed{Key::Q}), window_event(MouseButtonPressed{MouseButton::Right})});

    CHECK(router.device_input().key_down(Key::Q));
    CHECK(router.device_input().mouse_down(MouseButton::Right));
    CHECK_FALSE(router.gameplay_input().key_down(Key::Q));
    CHECK_FALSE(router.gameplay_input().mouse_down(MouseButton::Right));
}
