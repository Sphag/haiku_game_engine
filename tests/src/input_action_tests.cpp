// Action maps (US-011): buttons and axes, combined devices, dead zones, remapping, validation, and name tables. These are
// CPU-only tests. They feed synthetic events through InputState and ActionEvaluator on every operating system.

#include "engine/platform/action_map.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/input_names.hpp"
#include "engine/platform/input_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using engine::platform::ActionError;
using engine::platform::ActionErrorCode;
using engine::platform::ActionEvaluator;
using engine::platform::ActionId;
using engine::platform::ActionKind;
using engine::platform::ActionMap;
using engine::platform::apply_axis_dead_zone;
using engine::platform::Binding;
using engine::platform::GamepadAxis;
using engine::platform::GamepadAxisChanged;
using engine::platform::GamepadAxisSource;
using engine::platform::GamepadButton;
using engine::platform::GamepadButtonPressed;
using engine::platform::GamepadButtonReleased;
using engine::platform::GamepadButtonSource;
using engine::platform::GamepadConnected;
using engine::platform::InputEvent;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::KeyPressed;
using engine::platform::KeyReleased;
using engine::platform::KeySource;
using engine::platform::MouseAxis;
using engine::platform::MouseAxisSource;
using engine::platform::MouseButton;
using engine::platform::MouseButtonPressed;
using engine::platform::MouseButtonReleased;
using engine::platform::MouseButtonSource;
using engine::platform::MouseMoved;
using engine::platform::MouseWheelScrolled;
using engine::platform::WindowId;

namespace {

constexpr WindowId kWindow{1, 1};

// Keyboard and mouse events belong to the focused window. Gamepad events carry no window.
InputEvent window_event(engine::platform::InputEventData data) {
    return InputEvent{kWindow, std::move(data)};
}
InputEvent pad_event(engine::platform::InputEventData data) {
    return InputEvent{WindowId{}, std::move(data)};
}

Binding key_binding(Key key, float scale = 1.0f) {
    return Binding{KeySource{key}, scale, 0.0f};
}
Binding button_binding(MouseButton button) {
    return Binding{MouseButtonSource{button}, 1.0f, 0.0f};
}
Binding pad_button_binding(GamepadButton button, std::optional<std::uint32_t> slot = std::nullopt) {
    return Binding{GamepadButtonSource{button, slot}, 1.0f, 0.0f};
}
Binding pad_axis_binding(GamepadAxis axis, float scale = 1.0f, float dead_zone = 0.0f,
                         std::optional<std::uint32_t> slot = std::nullopt) {
    return Binding{GamepadAxisSource{axis, slot}, scale, dead_zone};
}

// One frame of input: begins the frame, applies the events, then evaluates the actions.
struct Harness {
    InputState input;
    ActionEvaluator actions;

    void frame(const ActionMap& map, const std::vector<InputEvent>& events) {
        input.begin_frame();
        for (const InputEvent& event : events) {
            input.apply(event);
        }
        actions.update(map, input);
    }
};

ActionId add(ActionMap& map, const char* name, ActionKind kind, std::vector<Binding> bindings) {
    const auto added = map.add_action(name, kind, std::move(bindings));
    REQUIRE(added.ok());
    return added.value();
}

}  // namespace

TEST_CASE("Action: a button reports its press, hold, and release edges", "[unit]") {
    ActionMap map;
    const ActionId jump = add(map, "jump", ActionKind::Button, {key_binding(Key::Space)});
    Harness run;

    run.frame(map, {window_event(KeyPressed{Key::Space})});
    CHECK(run.actions.down(jump));
    CHECK(run.actions.pressed(jump));
    CHECK_FALSE(run.actions.released(jump));
    CHECK(run.actions.value(jump) == 1.0f);

    run.frame(map, {});
    CHECK(run.actions.down(jump));
    CHECK_FALSE(run.actions.pressed(jump));

    run.frame(map, {window_event(KeyReleased{Key::Space})});
    CHECK_FALSE(run.actions.down(jump));
    CHECK(run.actions.released(jump));
    CHECK(run.actions.value(jump) == 0.0f);
}

TEST_CASE("Action: a key pair with opposite scales forms an axis", "[unit]") {
    ActionMap map;
    const ActionId move_x = add(map, "move_x", ActionKind::Axis,
                                {key_binding(Key::D, 1.0f), key_binding(Key::A, -1.0f)});
    Harness run;

    run.frame(map, {window_event(KeyPressed{Key::D})});
    CHECK(run.actions.value(move_x) == 1.0f);

    run.frame(map, {window_event(KeyPressed{Key::A})});
    CHECK(run.actions.value(move_x) == 0.0f);
    CHECK_FALSE(run.actions.down(move_x));

    run.frame(map, {window_event(KeyReleased{Key::D})});
    CHECK(run.actions.value(move_x) == -1.0f);
    CHECK(run.actions.down(move_x));
}

TEST_CASE("Action: one action reads keyboard, mouse, and gamepad at the same time", "[unit]") {
    ActionMap map;
    const ActionId fire = add(map, "fire", ActionKind::Button,
                              {key_binding(Key::Space), button_binding(MouseButton::Left), pad_button_binding(GamepadButton::A)});
    Harness run;
    run.frame(map, {pad_event(GamepadConnected{0})});

    run.frame(map, {window_event(KeyPressed{Key::Space}), window_event(MouseButtonPressed{MouseButton::Left}),
                    pad_event(GamepadButtonPressed{0, GamepadButton::A})});
    CHECK(run.actions.down(fire));
    CHECK(run.actions.pressed(fire));

    // Releasing one device does not release the action while another device still holds it.
    run.frame(map, {window_event(KeyReleased{Key::Space}), window_event(MouseButtonReleased{MouseButton::Left})});
    CHECK(run.actions.down(fire));
    CHECK_FALSE(run.actions.released(fire));

    run.frame(map, {pad_event(GamepadButtonReleased{0, GamepadButton::A})});
    CHECK_FALSE(run.actions.down(fire));
    CHECK(run.actions.released(fire));
}

TEST_CASE("Action: a gamepad binding with a slot ignores other pads", "[unit]") {
    ActionMap map;
    const ActionId confirm = add(map, "confirm", ActionKind::Button, {pad_button_binding(GamepadButton::B, 1)});
    Harness run;
    run.frame(map, {pad_event(GamepadConnected{0}), pad_event(GamepadConnected{1})});

    run.frame(map, {pad_event(GamepadButtonPressed{0, GamepadButton::B})});
    CHECK_FALSE(run.actions.down(confirm));

    run.frame(map, {pad_event(GamepadButtonPressed{1, GamepadButton::B})});
    CHECK(run.actions.down(confirm));
}

TEST_CASE("Action: an unslotted gamepad binding reads whichever pad is driving it", "[unit]") {
    ActionMap map;
    const ActionId confirm = add(map, "confirm", ActionKind::Button, {pad_button_binding(GamepadButton::Start)});
    Harness run;
    run.frame(map, {pad_event(GamepadConnected{2})});

    run.frame(map, {pad_event(GamepadButtonPressed{2, GamepadButton::Start})});
    CHECK(run.actions.down(confirm));
}

TEST_CASE("Action: stick dead zone reads 0 inside and rescales outside", "[unit]") {
    CHECK(apply_axis_dead_zone(0.1f, 0.2f) == 0.0f);
    CHECK(apply_axis_dead_zone(-0.2f, 0.2f) == 0.0f);
    CHECK(std::fabs(apply_axis_dead_zone(0.6f, 0.2f) - 0.5f) < 1e-5f);
    CHECK(std::fabs(apply_axis_dead_zone(-0.6f, 0.2f) + 0.5f) < 1e-5f);
    CHECK(apply_axis_dead_zone(1.0f, 0.2f) == 1.0f);
    // A zero dead zone leaves the value alone.
    CHECK(apply_axis_dead_zone(0.3f, 0.0f) == 0.3f);
}

TEST_CASE("Action: a stick axis applies its dead zone and scale in the evaluator", "[unit]") {
    ActionMap map;
    const ActionId move_y = add(map, "move_y", ActionKind::Axis,
                                {pad_axis_binding(GamepadAxis::LeftY, -1.0f, 0.2f)});
    Harness run;
    run.frame(map, {pad_event(GamepadConnected{0})});

    run.frame(map, {pad_event(GamepadAxisChanged{0, GamepadAxis::LeftY, 0.1f})});
    CHECK(run.actions.value(move_y) == 0.0f);
    CHECK_FALSE(run.actions.down(move_y));

    // Pushing the stick up gives +0.5 after the dead zone, and the -1 scale flips it.
    run.frame(map, {pad_event(GamepadAxisChanged{0, GamepadAxis::LeftY, 0.6f})});
    CHECK(std::fabs(run.actions.value(move_y) + 0.5f) < 1e-5f);
    CHECK(run.actions.down(move_y));
}

TEST_CASE("Action: a button threshold uses the trigger's analog value", "[unit]") {
    ActionMap map;
    const ActionId brake = add(map, "brake", ActionKind::Button, {pad_axis_binding(GamepadAxis::LeftTrigger)});
    Harness run;
    run.frame(map, {pad_event(GamepadConnected{0})});

    run.frame(map, {pad_event(GamepadAxisChanged{0, GamepadAxis::LeftTrigger, 0.4f})});
    CHECK_FALSE(run.actions.down(brake));

    run.frame(map, {pad_event(GamepadAxisChanged{0, GamepadAxis::LeftTrigger, 0.6f})});
    CHECK(run.actions.down(brake));
    CHECK(run.actions.pressed(brake));
}

TEST_CASE("Action: mouse motion scales into an axis and clamps to one", "[unit]") {
    ActionMap map;
    const ActionId look_x = add(map, "look_x", ActionKind::Axis,
                                {Binding{MouseAxisSource{MouseAxis::MotionX}, 0.1f, 0.0f}});
    Harness run;

    run.frame(map, {window_event(MouseMoved{5, 0})});
    CHECK(std::fabs(run.actions.value(look_x) - 0.5f) < 1e-5f);

    run.frame(map, {window_event(MouseMoved{50, 0})});
    CHECK(run.actions.value(look_x) == 1.0f);

    run.frame(map, {});
    CHECK(run.actions.value(look_x) == 0.0f);
}

TEST_CASE("Action: a wheel notch presses a button for one frame", "[unit]") {
    ActionMap map;
    const ActionId zoom = add(map, "zoom", ActionKind::Button,
                              {Binding{MouseAxisSource{MouseAxis::WheelY}, 1.0f, 0.0f}});
    Harness run;

    run.frame(map, {window_event(MouseWheelScrolled{0.0, 1.0})});
    CHECK(run.actions.pressed(zoom));

    run.frame(map, {});
    CHECK_FALSE(run.actions.down(zoom));
    CHECK(run.actions.released(zoom));
}

TEST_CASE("Action: remapping replaces the default, and reset restores it", "[unit]") {
    ActionMap map;
    const ActionId jump = add(map, "jump", ActionKind::Button, {key_binding(Key::Space)});
    Harness run;

    REQUIRE(map.rebind(jump, {key_binding(Key::Enter)}).ok());
    CHECK(map.has_override(jump));

    run.frame(map, {window_event(KeyPressed{Key::Space})});
    CHECK_FALSE(run.actions.down(jump));

    run.frame(map, {window_event(KeyReleased{Key::Space}), window_event(KeyPressed{Key::Enter})});
    CHECK(run.actions.down(jump));

    REQUIRE(map.reset(jump).ok());
    CHECK_FALSE(map.has_override(jump));
    run.frame(map, {window_event(KeyReleased{Key::Enter}), window_event(KeyPressed{Key::Space})});
    CHECK(run.actions.down(jump));
}

TEST_CASE("Action: an empty rebind unbinds the action, and it stays idle", "[unit]") {
    ActionMap map;
    const ActionId fire = add(map, "fire", ActionKind::Button, {button_binding(MouseButton::Left)});
    REQUIRE(map.rebind(fire, {}).ok());

    Harness run;
    run.frame(map, {window_event(MouseButtonPressed{MouseButton::Left})});
    CHECK_FALSE(run.actions.down(fire));
    CHECK(map.bindings(fire).empty());
}

TEST_CASE("Action: reset_all clears every override", "[unit]") {
    ActionMap map;
    const ActionId a = add(map, "a", ActionKind::Button, {key_binding(Key::A)});
    const ActionId b = add(map, "b", ActionKind::Button, {key_binding(Key::B)});
    REQUIRE(map.rebind(a, {key_binding(Key::C)}).ok());
    REQUIRE(map.rebind(b, {key_binding(Key::D)}).ok());

    map.reset_all();
    CHECK_FALSE(map.has_override(a));
    CHECK_FALSE(map.has_override(b));
    CHECK(map.overrides().empty());
}

TEST_CASE("Action: invalid definitions and bindings are rejected and change nothing", "[unit]") {
    ActionMap map;
    const ActionId jump = add(map, "jump", ActionKind::Button, {key_binding(Key::Space)});

    const auto empty = map.add_action("", ActionKind::Button, {});
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().code == ActionErrorCode::EmptyName);

    const auto duplicate = map.add_action("jump", ActionKind::Axis, {});
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error().code == ActionErrorCode::DuplicateName);

    // A dead zone on a key is meaningless, so it is refused.
    const auto key_dead_zone = map.add_action("dz", ActionKind::Button, {Binding{KeySource{Key::A}, 1.0f, 0.2f}});
    REQUIRE_FALSE(key_dead_zone.ok());
    CHECK(key_dead_zone.error().code == ActionErrorCode::InvalidBinding);
    CHECK_FALSE(map.find("dz").has_value());

    const std::vector<Binding> bad_bindings = {
        Binding{KeySource{Key::Unknown}, 1.0f, 0.0f},
        Binding{KeySource{Key::A}, 0.0f, 0.0f},
        Binding{KeySource{Key::A}, std::nanf(""), 0.0f},
        Binding{GamepadAxisSource{GamepadAxis::LeftX, 4u}, 1.0f, 0.0f},
        Binding{GamepadAxisSource{GamepadAxis::LeftX, std::nullopt}, 1.0f, 1.0f},
        Binding{GamepadButtonSource{GamepadButton::A, 4u}, 1.0f, 0.0f},
    };
    for (const Binding& bad : bad_bindings) {
        const auto result = map.rebind(jump, {bad});
        REQUIRE_FALSE(result.ok());
        CHECK(result.error().code == ActionErrorCode::InvalidBinding);
    }

    // None of the failed rebinds changed the action.
    CHECK_FALSE(map.has_override(jump));
    REQUIRE(map.bindings(jump).size() == 1);

    const auto unknown = map.rebind(ActionId{99}, {key_binding(Key::A)});
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().code == ActionErrorCode::UnknownAction);
}

TEST_CASE("Action: every key, button, and axis has a name that reads back", "[unit]") {
    using namespace engine::platform;

    for (std::size_t index = 1; index < kKeyCount; ++index) {
        const auto key = static_cast<Key>(index);
        REQUIRE(key_from_name(key_name(key)) == key);
    }
    for (std::size_t index = 0; index < kMouseButtonCount; ++index) {
        const auto button = static_cast<MouseButton>(index);
        REQUIRE(mouse_button_from_name(mouse_button_name(button)) == button);
    }
    for (std::size_t index = 0; index < kMouseAxisCount; ++index) {
        const auto axis = static_cast<MouseAxis>(index);
        REQUIRE(mouse_axis_from_name(mouse_axis_name(axis)) == axis);
    }
    for (std::size_t index = 0; index < kGamepadButtonCount; ++index) {
        const auto button = static_cast<GamepadButton>(index);
        REQUIRE(gamepad_button_from_name(gamepad_button_name(button)) == button);
    }
    for (std::size_t index = 0; index < kGamepadAxisCount; ++index) {
        const auto axis = static_cast<GamepadAxis>(index);
        REQUIRE(gamepad_axis_from_name(gamepad_axis_name(axis)) == axis);
    }

    CHECK_FALSE(key_from_name("").has_value());
    CHECK_FALSE(key_from_name("Unknown").has_value());
    CHECK_FALSE(key_from_name("space").has_value());  // names are case-sensitive, as the enum identifiers are
    CHECK_FALSE(gamepad_axis_from_name("Trigger").has_value());
}
