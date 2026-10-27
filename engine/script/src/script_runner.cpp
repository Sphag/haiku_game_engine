#include "engine/script/script_runner.hpp"

#include "script_bindings.hpp"
#include "script_state.hpp"

#include <algorithm>
#include <string>
#include <utility>

extern "C" {
#include "lauxlib.h"
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

// Tags the subscriptions made in its scope with `owner`, and restores the previous owner when it ends.
class OwnerScope {
public:
    OwnerScope(EventBus& events, std::uint64_t owner) : events_(events), previous_(events.set_owner(owner)) {}
    ~OwnerScope() {
        events_.set_owner(previous_);
    }

    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

private:
    EventBus& events_;
    std::uint64_t previous_;
};

}  // namespace

ScriptRunner::ScriptRunner(ScriptEngine& engine, const ecs::Registry& registry) : engine_(engine), registry_(registry) {}

ScriptRunner::~ScriptRunner() {
    for (Instance& instance : instances_) {
        release(instance);
    }
}

// The chunk runs in a new environment table, which reads fall through to the shared globals from. Its writes stay in the table, so
// two instances never share a global. The environment is referenced until the instance is released.
core::Result<int, ScriptError> ScriptRunner::load(std::string_view chunk_name, std::string_view source, std::uint64_t owner) {
    lua_State* state = engine_.state();
    const int base = lua_gettop(state);
    lua_newtable(state);  // the environment
    lua_newtable(state);
    lua_pushglobaltable(state);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_pushvalue(state, -1);
    const int env_ref = luaL_ref(state, LUA_REGISTRYINDEX);  // the environment stays on the stack for the chunk

    const std::string name = "=" + std::string(chunk_name);
    if (luaL_loadbufferx(state, source.data(), source.size(), name.c_str(), "t") != LUA_OK) {
        ScriptError error{ScriptErrorKind::Syntax, pop_message(state)};
        luaL_unref(state, LUA_REGISTRYINDEX, env_ref);
        lua_settop(state, base);
        return core::failure(std::move(error));
    }
    lua_pushvalue(state, -2);
    lua_setupvalue(state, -2, 1);  // the chunk's _ENV is its first upvalue

    bool ok = false;
    {
        OwnerScope scope(engine_.events(), owner);
        ok = lua_pcall(state, 0, 0, 0) == LUA_OK;
    }
    if (!ok) {
        ScriptError error{ScriptErrorKind::Runtime, pop_message(state)};
        engine_.events().release_owner(owner);
        luaL_unref(state, LUA_REGISTRYINDEX, env_ref);
        lua_settop(state, base);
        return core::failure(std::move(error));
    }
    lua_settop(state, base);
    return core::success(env_ref);
}

// Calls the callback `name` with the entity, and dt when given. A callback the chunk does not define is skipped. A raised error is
// queued as "<name>: <message>" and makes the call return false. The call does nothing when the entity is no longer alive.
bool ScriptRunner::call_entity(const Instance& instance, const char* name, const double* dt) {
    if (!registry_.world().alive(instance.entity)) {
        return true;
    }
    lua_State* state = engine_.state();
    const int base = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, instance.env_ref);
    lua_pushstring(state, name);
    lua_rawget(state, -2);  // rawget: a global of the same name in the shared table must not run
    lua_remove(state, -2);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, base);
        return true;
    }
    push_entity(state, instance.entity);
    int arguments = 1;
    if (dt != nullptr) {
        lua_pushnumber(state, *dt);
        arguments = 2;
    }
    OwnerScope scope(engine_.events(), instance.owner);
    if (lua_pcall(state, arguments, 0, 0) != LUA_OK) {
        engine_.events().report(ScriptError{ScriptErrorKind::Runtime, std::string(name) + ": " + pop_message(state)});
        lua_settop(state, base);
        return false;
    }
    lua_settop(state, base);
    return true;
}

// Calls export_state() when the old code defines it. Returns a registry reference to the value, or LUA_NOREF for nil, a missing
// export_state, or an error. An error is queued and the reload continues with fresh state.
int ScriptRunner::export_state(const Instance& instance) {
    lua_State* state = engine_.state();
    const int base = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, instance.env_ref);
    lua_pushstring(state, "export_state");
    lua_rawget(state, -2);
    lua_remove(state, -2);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, base);
        return LUA_NOREF;
    }
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
        engine_.events().report(ScriptError{ScriptErrorKind::Runtime, "export_state: " + pop_message(state)});
        lua_settop(state, base);
        return LUA_NOREF;
    }
    int ref = LUA_NOREF;
    if (!lua_isnil(state, -1)) {
        ref = luaL_ref(state, LUA_REGISTRYINDEX);
    }
    lua_settop(state, base);
    return ref;
}

// Calls import_state(value) on the new code when it defines the function. A value the new code does not import is dropped.
bool ScriptRunner::import_state(const Instance& instance, int value_ref) {
    lua_State* state = engine_.state();
    const int base = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, instance.env_ref);
    lua_pushstring(state, "import_state");
    lua_rawget(state, -2);
    lua_remove(state, -2);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, base);
        return true;
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, value_ref);
    if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
        engine_.events().report(ScriptError{ScriptErrorKind::Runtime, "import_state: " + pop_message(state)});
        lua_settop(state, base);
        return false;
    }
    lua_settop(state, base);
    return true;
}

// Drops an instance's event subscriptions and its environment reference. Callbacks do not run here.
void ScriptRunner::release(Instance& instance) {
    engine_.events().release_owner(instance.owner);
    luaL_unref(engine_.state(), LUA_REGISTRYINDEX, instance.env_ref);
    instance.env_ref = LUA_NOREF;
}

// Removes the instances whose entity has been destroyed, such as a streamed-out world, without a destroy callback.
void ScriptRunner::sweep_dead() {
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (registry_.world().alive(it->entity)) {
            ++it;
            continue;
        }
        release(*it);
        it = instances_.erase(it);
    }
}

ScriptRunner::Instance* ScriptRunner::find(ScriptId id) {
    const auto found = std::find_if(instances_.begin(), instances_.end(), [id](const Instance& entry) { return entry.id == id; });
    return found != instances_.end() ? &*found : nullptr;
}

core::Result<ScriptId, ScriptError> ScriptRunner::attach(ecs::EntityHandle entity, std::string_view chunk_name,
                                                          std::string_view source, ReloadPolicy policy) {
    if (!registry_.world().alive(entity)) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, "attach: the entity is not alive in the bound registry"});
    }
    const std::uint64_t owner = next_owner_++;
    auto loaded = load(chunk_name, source, owner);
    if (!loaded.ok()) {
        return core::failure(loaded.error());
    }
    const ScriptId id = next_id_++;
    instances_.push_back(Instance{id, entity, policy, InstanceState::Pending, owner, loaded.value()});
    return core::success(id);
}

bool ScriptRunner::detach(ScriptId id) {
    const auto found = std::find_if(instances_.begin(), instances_.end(), [id](const Instance& entry) { return entry.id == id; });
    if (found == instances_.end()) {
        return false;
    }
    if (found->state == InstanceState::Running) {
        call_entity(*found, "destroy", nullptr);
    }
    release(*found);
    instances_.erase(found);
    return true;
}

// The new code is loaded and checked before the old code is touched. Only then does the old code export, destroy, and release.
core::Result<core::Unit, ScriptError> ScriptRunner::reload(ScriptId id, std::string_view chunk_name, std::string_view source) {
    Instance* instance = find(id);
    if (instance == nullptr) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, "reload: no script instance has this id"});
    }
    const std::uint64_t owner = next_owner_++;
    auto loaded = load(chunk_name, source, owner);
    if (!loaded.ok()) {
        return core::failure(loaded.error());
    }

    // export_state and destroy belong to the old code, and they run only for a Running instance: a Pending one never started.
    int exported = LUA_NOREF;
    if (instance->state == InstanceState::Running) {
        if (instance->policy == ReloadPolicy::PreserveExports) {
            exported = export_state(*instance);
        }
        call_entity(*instance, "destroy", nullptr);
    }
    release(*instance);
    instance->env_ref = loaded.value();
    instance->owner = owner;
    instance->state = InstanceState::Pending;

    if (exported != LUA_NOREF) {
        if (!import_state(*instance, exported)) {
            instance->state = InstanceState::Faulted;
        }
        luaL_unref(engine_.state(), LUA_REGISTRYINDEX, exported);
    }
    return core::success();
}

void ScriptRunner::begin_frame() {
    sweep_dead();
    for (Instance& instance : instances_) {
        if (instance.state != InstanceState::Pending) {
            continue;
        }
        const bool started = call_entity(instance, "create", nullptr) && call_entity(instance, "start", nullptr);
        instance.state = started ? InstanceState::Running : InstanceState::Faulted;
    }
}

void ScriptRunner::fixed_update(double dt) {
    for (Instance& instance : instances_) {
        if (instance.state == InstanceState::Running && !call_entity(instance, "fixed_update", &dt)) {
            instance.state = InstanceState::Faulted;
        }
    }
}

void ScriptRunner::update(double dt) {
    for (Instance& instance : instances_) {
        if (instance.state == InstanceState::Running && !call_entity(instance, "update", &dt)) {
            instance.state = InstanceState::Faulted;
        }
    }
}

void ScriptRunner::stop_all() {
    for (Instance& instance : instances_) {
        if (instance.state == InstanceState::Running) {
            call_entity(instance, "destroy", nullptr);
        }
    }
    for (Instance& instance : instances_) {
        release(instance);
    }
    instances_.clear();
}

core::Result<std::string, ScriptError> ScriptRunner::capture_state(ScriptId id) {
    const Instance* instance = find(id);
    if (instance == nullptr) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, "capture_state: no instance has this id"});
    }
    return detail::encode_environment(engine_.state(), instance->env_ref);
}

core::Result<core::Unit, ScriptError> ScriptRunner::restore_state(ScriptId id, std::string_view state) {
    const Instance* instance = find(id);
    if (instance == nullptr) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, "restore_state: no instance has this id"});
    }
    if (instance->state != InstanceState::Pending) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, "restore_state: the instance has already started"});
    }
    return detail::decode_environment(engine_.state(), instance->env_ref, state);
}

std::optional<InstanceState> ScriptRunner::state(ScriptId id) const {
    const auto found = std::find_if(instances_.begin(), instances_.end(), [id](const Instance& entry) { return entry.id == id; });
    if (found == instances_.end()) {
        return std::nullopt;
    }
    return found->state;
}

std::vector<ScriptError> ScriptRunner::take_errors() {
    return engine_.take_errors();
}

}  // namespace engine::script
