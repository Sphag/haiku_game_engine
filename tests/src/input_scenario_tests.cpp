// Scripted input sessions (US-011). Each scenario runs frames of events through InputRouter and ActionEvaluator, the
// way the frame loop will, and checks the action values after every frame. The scenarios cover simultaneous devices,
// action-map changes during a session (remapping and a loaded profile), and editor capture followed by Play.

#include "engine/platform/action_map.hpp"
#include "engine/platform/action_profile.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/input_router.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using engine::platform::ActionEvaluator;
using engine::platform::ActionId;
using engine::platform::ActionKind;
using engine::platform::ActionMap;
using engine::platform::Binding;
using engine::platform::CaptureOwner;
using engine::platform::GamepadAxis;
using engine::platform::GamepadAxisChanged;
using engine::platform::GamepadAxisSource;
using engine::platform::GamepadButton;
using engine::platform::GamepadButtonPressed;
using engine::platform::GamepadButtonReleased;
using engine::platform::GamepadButtonSource;
using engine::platform::GamepadConnected;
using engine::platform::InputEvent;
using engine::platform::InputRouter;
using engine::platform::Key;
using engine::platform::KeyPressed;
using engine::platform::KeyReleased;
using engine::platform::KeySource;
using engine::platform::kCaptureKeyboard;
using engine::platform::MouseAxis;
using engine::platform::MouseAxisSource;
using engine::platform::MouseButton;
using engine::platform::MouseButtonPressed;
using engine::platform::MouseButtonReleased;
using engine::platform::MouseButtonSource;
using engine::platform::MouseMoved;
using engine::platform::RouterMode;
using engine::platform::WindowId;

namespace {

constexpr WindowId kWindow{1, 1};

InputEvent window_event(engine::platform::InputEventData data) {
    return InputEvent{kWindow, std::move(data)};
}
InputEvent pad_event(engine::platform::InputEventData data) {
    return InputEvent{WindowId{}, std::move(data)};
}

// One running session: the map, the router that splits input, and the evaluator that reads gameplay's view.
struct Session {
    ActionMap map;
    InputRouter router;
    ActionEvaluator actions;

    void frame(const std::vector<InputEvent>& events) {
        router.route(events);
        actions.update(map, router.gameplay_input());
    }
    ActionId id(const char* name) const {
        return *map.find(name);
    }
};

// The controls used by every scenario: jump is held by any of three devices, move_x is a key pair plus a stick, and
// look_x is mouse motion.
void define_controls(Session& session) {
    REQUIRE(session.map
                .add_action("jump", ActionKind::Button,
                            {Binding{KeySource{Key::Space}, 1.0f, 0.0f},
                             Binding{MouseButtonSource{MouseButton::Left}, 1.0f, 0.0f},
                             Binding{GamepadButtonSource{GamepadButton::A, std::nullopt}, 1.0f, 0.0f}})
                .ok());
    REQUIRE(session.map
                .add_action("move_x", ActionKind::Axis,
                            {Binding{KeySource{Key::D}, 1.0f, 0.0f}, Binding{KeySource{Key::A}, -1.0f, 0.0f},
                             Binding{GamepadAxisSource{GamepadAxis::LeftX, 0u}, 1.0f, 0.2f}})
                .ok());
    REQUIRE(session.map
                .add_action("look_x", ActionKind::Axis, {Binding{MouseAxisSource{MouseAxis::MotionX}, 0.1f, 0.0f}})
                .ok());
}

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 1e-5f;
}

}  // namespace

TEST_CASE("Scenario: keyboard, gamepad, and mouse drive actions in the same frames", "[integration]") {
    Session session;
    define_controls(session);
    const ActionId jump = session.id("jump");
    const ActionId move_x = session.id("move_x");
    const ActionId look_x = session.id("look_x");

    session.frame({pad_event(GamepadConnected{0})});
    CHECK_FALSE(session.actions.down(jump));

    // Three devices at once: the key sets move_x to -1 with the stick full forward, the pad's A and the mouse click
    // and the held key all press jump, and the mouse moves.
    session.frame({window_event(KeyPressed{Key::D}), pad_event(GamepadAxisChanged{0, GamepadAxis::LeftX, 0.9f}),
                   pad_event(GamepadButtonPressed{0, GamepadButton::A}), window_event(MouseButtonPressed{MouseButton::Left}),
                   window_event(MouseMoved{10, 0})});
    // move_x: the key gives +1, the stick gives (0.9 - 0.2) / 0.8 = 0.875, and the sum clamps to 1.
    CHECK(near(session.actions.value(move_x), 1.0f));
    CHECK(session.actions.pressed(jump));
    CHECK(near(session.actions.value(look_x), 1.0f));

    // The key is released while the stick still pushes: move_x falls to the stick alone, and jump stays down because
    // the pad and the mouse still hold it. Space is pressed on top, which does not repeat the press edge.
    session.frame({window_event(KeyReleased{Key::D}), window_event(KeyPressed{Key::Space}),
                   window_event(MouseMoved{-5, 0})});
    CHECK(near(session.actions.value(move_x), 0.875f));
    CHECK(session.actions.down(jump));
    CHECK_FALSE(session.actions.pressed(jump));
    CHECK(near(session.actions.value(look_x), -0.5f));

    // The stick returns to center inside its dead zone, and every device releases jump on the same frame.
    session.frame({pad_event(GamepadAxisChanged{0, GamepadAxis::LeftX, 0.1f}),
                   pad_event(GamepadButtonReleased{0, GamepadButton::A}), window_event(KeyReleased{Key::Space}),
                   window_event(MouseButtonReleased{MouseButton::Left})});
    CHECK(near(session.actions.value(move_x), 0.0f));
    CHECK_FALSE(session.actions.down(jump));
    CHECK(session.actions.released(jump));
}

TEST_CASE("Scenario: remapping and a loaded profile change controls during a session", "[integration]") {
    Session session;
    define_controls(session);
    const ActionId jump = session.id("jump");

    session.frame({window_event(KeyPressed{Key::Space})});
    REQUIRE(session.actions.down(jump));
    session.frame({window_event(KeyReleased{Key::Space})});

    // The player moves jump to Enter. Space no longer jumps; Enter does.
    REQUIRE(session.map.rebind(jump, {Binding{KeySource{Key::Enter}, 1.0f, 0.0f}}).ok());
    session.frame({window_event(KeyPressed{Key::Space})});
    CHECK_FALSE(session.actions.down(jump));
    session.frame({window_event(KeyReleased{Key::Space}), window_event(KeyPressed{Key::Enter})});
    CHECK(session.actions.pressed(jump));

    // The profile is saved, the session resets, and the saved profile is loaded again in a new session. The jump is
    // still Enter, and move_x keeps its stick binding.
    const std::string saved = engine::platform::serialize_profile(session.map.overrides());
    session.frame({window_event(KeyReleased{Key::Enter})});
    session.map.reset_all();
    session.frame({window_event(KeyPressed{Key::Enter})});
    CHECK_FALSE(session.actions.down(jump));

    const auto parsed = engine::platform::parse_profile(saved);
    REQUIRE(parsed.ok());
    REQUIRE(session.map.apply_overrides(parsed.value()).ok());
    session.frame({window_event(KeyReleased{Key::Enter}), window_event(KeyPressed{Key::Enter})});
    CHECK(session.actions.pressed(jump));
    CHECK(session.map.has_override(jump));
    CHECK_FALSE(session.map.has_override(session.id("move_x")));
}

TEST_CASE("Scenario: editor capture holds gameplay back, and Play hands every device over", "[integration]") {
    Session session;
    define_controls(session);
    const ActionId jump = session.id("jump");

    session.frame({window_event(KeyPressed{Key::Space})});
    REQUIRE(session.actions.down(jump));

    // The editor takes the keyboard while Space is still held. Gameplay sees the release at the capture start.
    REQUIRE(session.router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));
    session.frame({});
    CHECK_FALSE(session.actions.down(jump));
    CHECK(session.actions.released(jump));

    // While captured, the editor's presses and releases do not reach gameplay, even with the mouse free.
    session.frame({window_event(KeyReleased{Key::Space}), window_event(KeyPressed{Key::Space})});
    CHECK_FALSE(session.actions.down(jump));
    CHECK(session.router.captured_events().size() == 2);

    // Play releases the capture, and the still-held Space is pressed for gameplay on the first frame of Play.
    session.router.set_mode(RouterMode::Play);
    CHECK_FALSE(session.router.set_capture(CaptureOwner::Editor, kCaptureKeyboard));
    session.frame({});
    CHECK(session.actions.pressed(jump));
    CHECK(session.actions.down(jump));

    // Gameplay keeps every event in Play. Key releases arrive and reach jump like any other input.
    session.frame({window_event(KeyReleased{Key::Space})});
    CHECK(session.actions.released(jump));
    CHECK(session.router.captured_events().empty());
}
