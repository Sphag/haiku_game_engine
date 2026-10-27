#include "engine/script/script_engine.hpp"

#include "script_bindings.hpp"

#include "engine/audio/audio_engine.hpp"

#include <string>
#include <utility>

extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}

namespace engine::script {
namespace {

// Pops the error message at the top of the stack. Lua may raise a non-string error object; it is reported as such.
std::string pop_message(lua_State* state) {
    const char* text = lua_tostring(state, -1);
    std::string message = text != nullptr ? text : "error object is not a string";
    lua_pop(state, 1);
    return message;
}

// The safe subset: the base library without the file loaders, plus coroutine, table, string, math, and utf8.
void open_safe_libraries(lua_State* state) {
    const luaL_Reg libraries[] = {
        {"_G", luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const luaL_Reg& library : libraries) {
        luaL_requiref(state, library.name, library.func, 1);
        lua_pop(state, 1);
    }
    lua_pushnil(state);
    lua_setglobal(state, "dofile");
    lua_pushnil(state);
    lua_setglobal(state, "loadfile");
}

}  // namespace

ScriptEngine::ScriptEngine() : state_(luaL_newstate()), events_(state_) {
    open_safe_libraries(state_);
    install_bindings(state_, *this);
}

ScriptEngine::~ScriptEngine() {
    if (services_.audio != nullptr) {
        services_.audio->stop_owner(audio_owner());
    }
    lua_close(state_);
}

core::Result<core::Unit, ScriptError> ScriptEngine::run(std::string_view chunk_name, std::string_view source) {
    // A leading '=' names the chunk exactly, without the [string "..."] form Lua uses for unnamed sources.
    const std::string name = "=" + std::string(chunk_name);
    if (luaL_loadbufferx(state_, source.data(), source.size(), name.c_str(), "t") != LUA_OK) {
        return core::failure(ScriptError{ScriptErrorKind::Syntax, pop_message(state_)});
    }
    if (lua_pcall(state_, 0, 0, 0) != LUA_OK) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, pop_message(state_)});
    }
    return core::success(core::Unit{});
}

int ScriptEngine::stack_depth() const {
    return lua_gettop(state_);
}

}  // namespace engine::script
