#pragma once

// Installs the gameplay modules (entity, transform, input, assets, physics, events) into a Lua state. Internal to engine_script.

#include "engine/ecs/entity.hpp"

struct lua_State;

namespace engine::script {

class ScriptEngine;

// Pushes an entity userdata for `handle`. The script runner uses it to pass the entity to lifecycle callbacks (US-056).
void push_entity(lua_State* state, ecs::EntityHandle handle);

// Every module function reads the engine as its upvalue, so the state holds no other reference to it.
void install_bindings(lua_State* state, ScriptEngine& engine);

}  // namespace engine::script
