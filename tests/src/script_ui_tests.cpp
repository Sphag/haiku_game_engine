// Lua callbacks for the game UI (US-058). Button and slider callbacks run Lua functions through the real VM: the functions run on
// activation, an error is reported without stopping the host, the Lua stack stays balanced, and a function is released with the
// last copy of its callback.

#include "engine/script/script_engine.hpp"
#include "engine/script/script_ui.hpp"
#include "engine/platform/input_router.hpp"
#include "engine/ui/ui.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

extern "C" {
#include "lua.h"
}

#include <string>
#include <utility>

using engine::platform::InputEvent;
using engine::platform::Key;
using engine::platform::WindowId;
using engine::script::ScriptEngine;
using engine::ui::Anchor;
using engine::ui::ElementKind;
using engine::ui::ElementSpec;
using engine::ui::kNoElement;
using engine::ui::Ui;

namespace {

// Reads a global number from the engine's Lua state.
double global_number(ScriptEngine& engine, const char* name) {
    lua_State* state = engine.state();
    lua_getglobal(state, name);
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    return value;
}

// Leaves the global function `name` on the Lua stack, and returns its stack index as seen from the top.
void push_global(ScriptEngine& engine, const char* name) {
    lua_getglobal(engine.state(), name);
}

}  // namespace

TEST_CASE("A button callback runs the Lua function on activation and keeps the stack balanced", "[unit]") {
    ScriptEngine engine;
    REQUIRE(engine.run("ui_test", "clicks = 0\nfunction on_play() clicks = clicks + 1 end").ok());
    push_global(engine, "on_play");
    auto action = engine::script::ui_action(engine, -1);
    lua_pop(engine.state(), 1);
    REQUIRE(static_cast<bool>(action));
    CHECK(engine.stack_depth() == 0);

    action();
    action();
    CHECK(global_number(engine, "clicks") == 2.0);
    CHECK(engine.stack_depth() == 0);
    CHECK(engine.take_errors().empty());
}

TEST_CASE("A button in the UI runs its Lua callback when it is clicked", "[unit]") {
    ScriptEngine engine;
    REQUIRE(engine.run("ui_test", "pressed = 0\nfunction on_click() pressed = pressed + 1 end").ok());

    Ui ui;
    ui.set_viewport(1280, 720);
    ui.set_active(true);
    ElementSpec button;
    button.kind = ElementKind::Button;
    button.placement = engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 100.0f, 50.0f);
    push_global(engine, "on_click");
    button.on_click = engine::script::ui_action(engine, -1);
    lua_pop(engine.state(), 1);
    REQUIRE(ui.add(kNoElement, std::move(button)).ok());

    ui.set_pointer(10.0f, 10.0f);
    (void)ui.handle({InputEvent{WindowId{}, engine::platform::MouseButtonPressed{engine::platform::MouseButton::Left}}});
    (void)ui.handle({InputEvent{WindowId{}, engine::platform::MouseButtonReleased{engine::platform::MouseButton::Left}}});
    CHECK(global_number(engine, "pressed") == 1.0);

    // Keyboard activation runs the same callback.
    (void)ui.handle({InputEvent{WindowId{}, engine::platform::KeyPressed{Key::Tab}}});
    (void)ui.handle({InputEvent{WindowId{}, engine::platform::KeyPressed{Key::Enter}}});
    CHECK(global_number(engine, "pressed") == 2.0);
}

TEST_CASE("A slider callback receives the new value as a Lua number", "[unit]") {
    ScriptEngine engine;
    REQUIRE(engine.run("ui_test", "volume = -1\nfunction on_volume(v) volume = v end").ok());
    push_global(engine, "on_volume");
    auto action = engine::script::ui_value_action(engine, -1);
    lua_pop(engine.state(), 1);

    action(0.25f);
    CHECK_THAT(global_number(engine, "volume"), Catch::Matchers::WithinAbs(0.25, 1e-6));
    CHECK(engine.stack_depth() == 0);
}

TEST_CASE("A raised error in a UI callback is reported, and the host keeps running", "[unit]") {
    ScriptEngine engine;
    REQUIRE(engine.run("ui_test", "function fail() error('menu failed') end\nfunction ok() count = 1 end").ok());
    push_global(engine, "fail");
    auto fail = engine::script::ui_action(engine, -1);
    lua_pop(engine.state(), 1);
    push_global(engine, "ok");
    auto good = engine::script::ui_action(engine, -1);
    lua_pop(engine.state(), 1);

    fail();
    good();
    CHECK(global_number(engine, "count") == 1.0);
    CHECK(engine.stack_depth() == 0);

    const auto errors = engine.take_errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message.find("menu failed") != std::string::npos);
}

TEST_CASE("A value that is not a function is reported and gives a callback that does nothing", "[unit]") {
    ScriptEngine engine;
    lua_pushinteger(engine.state(), 7);
    auto action = engine::script::ui_action(engine, -1);
    lua_pop(engine.state(), 1);

    CHECK_FALSE(static_cast<bool>(action));
    const auto errors = engine.take_errors();
    REQUIRE(errors.size() == 1);
    CHECK(engine.stack_depth() == 0);
}
