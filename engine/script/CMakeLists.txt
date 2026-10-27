# Lua gameplay API (US-055). The Lua 5.4 VM is built from the vendored sources as the lua_vm target (C, outside the first-party
# warning policy). The in-house C API bindings are engine_script: they expose entities, transforms, input actions, assets, physics
# queries, and events to scripts, and every error a script can hit comes back as a controlled Lua error. The contract is in
# engine/script/include/engine/script/script_engine.hpp.
set(LUA_REPO_ROOT ${PROJECT_SOURCE_DIR}/third_party/lua)

# The standard Lua library files. lua.c and luac.c (the command-line programs) are not vendored.
set(LUA_SOURCES
  lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c
  linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c
  lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c
)
list(TRANSFORM LUA_SOURCES PREPEND ${LUA_REPO_ROOT}/)

add_library(lua_vm STATIC ${LUA_SOURCES})
target_include_directories(lua_vm SYSTEM PUBLIC ${LUA_REPO_ROOT})
set_target_properties(lua_vm PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

add_library(engine_script STATIC
  src/script_engine.cpp
  src/event_bus.cpp
  src/script_bindings.cpp
  src/script_runner.cpp
  src/script_ui.cpp
  src/script_state.cpp
)
target_include_directories(engine_script PUBLIC include)
target_link_libraries(engine_script PUBLIC engine_core engine_ecs engine_scene engine_assets engine_platform engine_physics engine_audio engine_animation lua_vm)
# Instance state is written and read as JSON with the vendored nlohmann/json, a system include (see script_state.cpp).
target_include_directories(engine_script SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/single_include)
engine_configure_first_party(engine_script)
