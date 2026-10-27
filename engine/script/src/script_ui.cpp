#include "engine/script/script_ui.hpp"

#include <memory>
#include <utility>

extern "C" {
#include "lauxlib.h"
}

namespace engine::script {

namespace {

// Owns one registry reference. Copies of a callback share it, and the reference is released with the last copy.
class Handler {
public:
    Handler(ScriptEngine& owner, lua_State* lua, int registry_ref) noexcept : engine_(&owner), state_(lua), ref_(registry_ref) {}
    ~Handler() {
        luaL_unref(state_, LUA_REGISTRYINDEX, ref_);
    }
    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;

    [[nodiscard]] ScriptEngine& engine() const noexcept {
        return *engine_;
    }
    [[nodiscard]] lua_State* state() const noexcept {
        return state_;
    }
    [[nodiscard]] int ref() const noexcept {
        return ref_;
    }

private:
    ScriptEngine* engine_;
    lua_State* state_;
    int ref_;
};

// Stores the function at `index` in the registry. Null when the value there is not a function.
std::shared_ptr<const Handler> make_handler(ScriptEngine& engine, int index) {
    lua_State* state = engine.state();
    const int absolute = lua_absindex(state, index);
    if (lua_type(state, absolute) != LUA_TFUNCTION) {
        engine.events().report(ScriptError{ScriptErrorKind::Runtime, "ui callback expects a Lua function"});
        return nullptr;
    }
    lua_pushvalue(state, absolute);
    const int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    return std::make_shared<const Handler>(engine, state, ref);
}

// Calls the handler with `arguments` values, which `push` leaves on the stack. A raised error goes to the engine's events.
template <typename Push>
void call_handler(const Handler& handler, int arguments, Push&& push) {
    lua_State* state = handler.state();
    lua_rawgeti(state, LUA_REGISTRYINDEX, handler.ref());
    push(state);
    if (lua_pcall(state, arguments, 0, 0) != LUA_OK) {
        const char* text = lua_tostring(state, -1);
        handler.engine().events().report(ScriptError{ScriptErrorKind::Runtime, text != nullptr ? text : "error object is not a string"});
        lua_pop(state, 1);
    }
}

}  // namespace

std::function<void()> ui_action(ScriptEngine& engine, int index) {
    std::shared_ptr<const Handler> handler = make_handler(engine, index);
    if (!handler) {
        return {};
    }
    return [handler = std::move(handler)]() {
        call_handler(*handler, 0, [](lua_State*) {});
    };
}

std::function<void(float)> ui_value_action(ScriptEngine& engine, int index) {
    std::shared_ptr<const Handler> handler = make_handler(engine, index);
    if (!handler) {
        return {};
    }
    return [handler = std::move(handler)](float value) {
        call_handler(*handler, 1, [value](lua_State* state) { lua_pushnumber(state, static_cast<lua_Number>(value)); });
    };
}

}  // namespace engine::script
