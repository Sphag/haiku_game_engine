#pragma once

// The Lua gameplay API (US-055). A ScriptEngine owns one Lua 5.4 state with the safe standard libraries (base, coroutine, table,
// string, math, utf8; no io, os, package, or debug) and six modules that scripts call:
//
//   entity     create, destroy, alive, has/add/remove of a component by name
//   transform  get and set a live entity's scene::Transform (position, rotation as w,x,y,z, scale)
//   input      value, down, pressed, released for a named action of the bound ActionMap
//   assets     find by source path, exists, path, importer, dependencies, by GUID text
//   physics    raycast and overlap_sphere on the bound PhysicsWorld, with an optional filter table
//   events     on(name, fn) -> id, off(id), emit(name, payload)
//   audio      load(path) -> clip, play(clip, opts) and play_at(clip, x, y, z, opts) -> voice, stop, is_playing, set_position,
//              set_listener, set_bus_volume, pause, play_music(path, opts), stop_music (US-057)
//   animation  set_float(machine, param, value), set_bool(machine, param, value), trigger(machine, param), state(machine) -> name,
//              get_float, get_bool, get_trigger, blending(machine) for the named state machines in the bound AnimatorSet (US-069)
//
// Scripts never hold C++ pointers. An entity is a Lua userdata that stores the ecs handle by value, and every call checks it against the
// bound registry, so a destroyed entity raises a Lua error instead of touching freed storage. Services are bound by pointer: the caller
// keeps them alive for as long as the engine can run a script.
//
// Every failure a script can cause comes back as a ScriptError or as a Lua error caught by run(); a host call never aborts the process.
// The engine is not thread-safe; the owning thread runs scripts.

#include "engine/assets/asset_registry.hpp"
#include "engine/core/result.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/platform/action_map.hpp"
#include "engine/script/event_bus.hpp"
#include "engine/script/script_error.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

struct lua_State;

namespace engine::audio {
class AudioEngine;
}

namespace engine::animation {
class AnimatorSet;
}

namespace engine::script {

// The engine services scripts read and write. A null service makes the calls that need it raise "no X is bound".
struct ScriptServices {
    ecs::Registry* registry = nullptr;
    const platform::ActionMap* actions = nullptr;
    const platform::ActionEvaluator* input = nullptr;
    const assets::AssetRegistry* assets = nullptr;
    physics::PhysicsWorld* physics = nullptr;
    audio::AudioEngine* audio = nullptr;
    animation::AnimatorSet* animators = nullptr;
};

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;
    ScriptEngine(ScriptEngine&&) = delete;
    ScriptEngine& operator=(ScriptEngine&&) = delete;

    void bind(const ScriptServices& services) noexcept {
        services_ = services;
    }
    [[nodiscard]] const ScriptServices& services() const noexcept {
        return services_;
    }

    // Compiles and runs a chunk named `chunk_name`. A syntax error or a raised error comes back with Lua's message. The Lua stack is
    // empty again on every return, success or failure.
    [[nodiscard]] core::Result<core::Unit, ScriptError> run(std::string_view chunk_name, std::string_view source);

    [[nodiscard]] EventBus& events() noexcept {
        return events_;
    }
    // The Lua state, for the script runner (US-056). Scripts never reach it; the host uses run() and the runner.
    [[nodiscard]] lua_State* state() noexcept {
        return state_;
    }
    // Makes `entity` available to scripts as the global `name`. Scripts then compare it with the entities they receive.
    void set_global_entity(std::string_view name, ecs::EntityHandle entity);

    // Errors that event handlers raised since the last call, in the order they happened.
    [[nodiscard]] std::vector<ScriptError> take_errors() {
        return events_.take_errors();
    }

    // The owner tag of the audio voices this engine starts. The destructor stops them, so a script that goes away leaves no sound behind.
    [[nodiscard]] std::uint64_t audio_owner() const noexcept {
        return reinterpret_cast<std::uint64_t>(this);
    }

    // Number of values on the Lua main stack. Between calls it is zero, so a test can check that no call leaked a value.
    [[nodiscard]] int stack_depth() const;

private:
    lua_State* state_ = nullptr;
    ScriptServices services_;
    EventBus events_;
};

}  // namespace engine::script
