// The Lua C API bindings (US-055). Rules that every function here follows:
//  - A script error is raised with luaL_error or luaL_argerror. Those longjmp out of the C++ frame, so no function that can raise
//    holds a std::string or another object with a destructor across the raise. Owning values are built only after the checks.
//  - A function reads its arguments and validates them before it touches the engine, so a bad call changes nothing.
//  - Entities cross into Lua as an ecs::EntityHandle value in a userdata. Every use checks the handle against the bound registry.

#include "script_bindings.hpp"

#include "engine/animation/state_machine.hpp"
#include "engine/assets/asset_registry.hpp"
#include "engine/audio/audio_engine.hpp"
#include "engine/core/path.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/platform/action_map.hpp"
#include "engine/scene/transform.hpp"
#include "engine/script/script_engine.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "lauxlib.h"
}

namespace engine::script {
namespace {

constexpr const char* kEntityMeta = "engine.Entity";

ScriptEngine& engine_of(lua_State* state) {
    return *static_cast<ScriptEngine*>(lua_touserdata(state, lua_upvalueindex(1)));
}

// Each service getter raises a controlled error when the service is missing, so a script never reaches a null pointer.
ecs::Registry& bound_registry(lua_State* state) {
    ecs::Registry* registry = engine_of(state).services().registry;
    if (registry == nullptr) {
        luaL_error(state, "no entity registry is bound");
    }
    return *registry;
}

const platform::ActionMap& bound_actions(lua_State* state) {
    const platform::ActionMap* actions = engine_of(state).services().actions;
    if (actions == nullptr) {
        luaL_error(state, "no action map is bound");
    }
    return *actions;
}

const platform::ActionEvaluator& bound_input(lua_State* state) {
    const platform::ActionEvaluator* input = engine_of(state).services().input;
    if (input == nullptr) {
        luaL_error(state, "no input evaluator is bound");
    }
    return *input;
}

const assets::AssetRegistry& bound_assets(lua_State* state) {
    const assets::AssetRegistry* registry = engine_of(state).services().assets;
    if (registry == nullptr) {
        luaL_error(state, "no asset registry is bound");
    }
    return *registry;
}

physics::PhysicsWorld& bound_physics(lua_State* state) {
    physics::PhysicsWorld* world = engine_of(state).services().physics;
    if (world == nullptr) {
        luaL_error(state, "no physics world is bound");
    }
    return *world;
}

// The handle in argument `index`, without a liveness check. Raises a Lua argument error for anything that is not an entity.
ecs::EntityHandle entity_value(lua_State* state, int index) {
    return *static_cast<ecs::EntityHandle*>(luaL_checkudata(state, index, kEntityMeta));
}

// The handle in argument `index`, which must name a live entity of the bound registry.
ecs::EntityHandle live_entity(lua_State* state, int index, const ecs::Registry& registry) {
    const ecs::EntityHandle entity = entity_value(state, index);
    if (!registry.world().alive(entity)) {
        luaL_error(state, "entity is destroyed or belongs to another world");
    }
    return entity;
}

void push_numbers(lua_State* state, const double* values, int count) {
    lua_createtable(state, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(state, values[i]);
        lua_rawseti(state, -2, i + 1);
    }
}

// Reads `count` finite numbers from the table at `index`. False when an entry is missing, not a number, or not finite.
bool read_numbers(lua_State* state, int index, double* out, int count) {
    if (!lua_istable(state, index)) {
        return false;
    }
    const int table = lua_absindex(state, index);  // lua_geti pushes, so a relative index would drift
    for (int i = 0; i < count; ++i) {
        lua_geti(state, table, i + 1);
        int is_number = 0;
        const lua_Number value = lua_tonumberx(state, -1, &is_number);
        lua_pop(state, 1);
        if (is_number == 0 || !std::isfinite(value)) {
            return false;
        }
        out[i] = value;
    }
    return true;
}

// Reads table[field] as `count` numbers. Returns 0 when the field is absent, 1 when it was read, and -1 when it is malformed.
int read_field(lua_State* state, int table, const char* field, double* out, int count) {
    lua_getfield(state, table, field);
    int status = 0;
    if (!lua_isnil(state, -1)) {
        status = read_numbers(state, -1, out, count) ? 1 : -1;
    }
    lua_pop(state, 1);
    return status;
}

// Reads `count` consecutive number arguments from `first`. Each one must be finite.
void number_args(lua_State* state, int first, double* out, int count) {
    for (int i = 0; i < count; ++i) {
        out[i] = luaL_checknumber(state, first + i);
        if (!std::isfinite(out[i])) {
            luaL_argerror(state, first + i, "expected a finite number");
        }
    }
}

// ---- entity -----------------------------------------------------------------------------------------------------------

enum class Component { Transform };

bool component_named(const char* name, Component& out) {
    if (std::strcmp(name, "transform") == 0) {
        out = Component::Transform;
        return true;
    }
    return false;
}

bool has_component(const ecs::Registry& registry, ecs::EntityHandle entity, Component component) {
    switch (component) {
        case Component::Transform:
            return registry.has<scene::Transform>(entity);
    }
    return false;
}

int entity_create(lua_State* state) {
    push_entity(state, bound_registry(state).create());
    return 1;
}

int entity_destroy(lua_State* state) {
    ecs::Registry& registry = bound_registry(state);
    registry.destroy(live_entity(state, 1, registry));
    return 0;
}

int entity_alive(lua_State* state) {
    const ecs::Registry& registry = bound_registry(state);
    lua_pushboolean(state, registry.world().alive(entity_value(state, 1)));
    return 1;
}

int entity_has(lua_State* state) {
    const ecs::Registry& registry = bound_registry(state);
    const ecs::EntityHandle entity = live_entity(state, 1, registry);
    Component component{};
    if (!component_named(luaL_checkstring(state, 2), component)) {
        return luaL_argerror(state, 2, "unknown component");
    }
    lua_pushboolean(state, has_component(registry, entity, component));
    return 1;
}

int entity_add(lua_State* state) {
    ecs::Registry& registry = bound_registry(state);
    const ecs::EntityHandle entity = live_entity(state, 1, registry);
    Component component{};
    if (!component_named(luaL_checkstring(state, 2), component)) {
        return luaL_argerror(state, 2, "unknown component");
    }
    // Adding a component the entity already has changes nothing and returns false.
    bool added = false;
    switch (component) {
        case Component::Transform:
            added = registry.add<scene::Transform>(entity) != nullptr;
            break;
    }
    lua_pushboolean(state, added);
    return 1;
}

int entity_remove(lua_State* state) {
    ecs::Registry& registry = bound_registry(state);
    const ecs::EntityHandle entity = live_entity(state, 1, registry);
    Component component{};
    if (!component_named(luaL_checkstring(state, 2), component)) {
        return luaL_argerror(state, 2, "unknown component");
    }
    bool removed = false;
    switch (component) {
        case Component::Transform:
            removed = registry.remove<scene::Transform>(entity);
            break;
    }
    lua_pushboolean(state, removed);
    return 1;
}

int entity_equal(lua_State* state) {
    lua_pushboolean(state, entity_value(state, 1) == entity_value(state, 2));
    return 1;
}

int entity_tostring(lua_State* state) {
    const ecs::EntityHandle entity = entity_value(state, 1);
    lua_pushfstring(state, "entity(%I:%I)", static_cast<lua_Integer>(entity.index), static_cast<lua_Integer>(entity.generation));
    return 1;
}

// ---- transform --------------------------------------------------------------------------------------------------------

int transform_get(lua_State* state) {
    ecs::Registry& registry = bound_registry(state);
    const ecs::EntityHandle entity = live_entity(state, 1, registry);
    const scene::Transform* transform = registry.get<scene::Transform>(entity);
    if (transform == nullptr) {
        return luaL_error(state, "entity has no transform component");
    }
    const double position[3] = {transform->position.x, transform->position.y, transform->position.z};
    const double rotation[4] = {transform->rotation.w, transform->rotation.x, transform->rotation.y, transform->rotation.z};
    const double scale[3] = {transform->scale.x, transform->scale.y, transform->scale.z};
    lua_createtable(state, 0, 3);
    push_numbers(state, position, 3);
    lua_setfield(state, -2, "position");
    push_numbers(state, rotation, 4);
    lua_setfield(state, -2, "rotation");
    push_numbers(state, scale, 3);
    lua_setfield(state, -2, "scale");
    return 1;
}

// Sets any of position, rotation (w, x, y, z), and scale. A missing field keeps its value. The rotation is normalised. A malformed
// field raises an error and leaves the component as it was.
int transform_set(lua_State* state) {
    ecs::Registry& registry = bound_registry(state);
    const ecs::EntityHandle entity = live_entity(state, 1, registry);
    luaL_checktype(state, 2, LUA_TTABLE);
    scene::Transform* current = registry.get<scene::Transform>(entity);
    if (current == nullptr) {
        return luaL_error(state, "entity has no transform component");
    }
    double position[3] = {0.0, 0.0, 0.0};
    double rotation[4] = {1.0, 0.0, 0.0, 0.0};
    double scale[3] = {1.0, 1.0, 1.0};
    const int position_status = read_field(state, 2, "position", position, 3);
    const int rotation_status = read_field(state, 2, "rotation", rotation, 4);
    const int scale_status = read_field(state, 2, "scale", scale, 3);
    if (position_status < 0) {
        return luaL_error(state, "transform.position must be three finite numbers");
    }
    if (rotation_status < 0) {
        return luaL_error(state, "transform.rotation must be four finite numbers (w, x, y, z)");
    }
    if (scale_status < 0) {
        return luaL_error(state, "transform.scale must be three finite numbers");
    }
    const glm::dquat rotation_value(rotation[0], rotation[1], rotation[2], rotation[3]);
    if (rotation_status > 0 && glm::length(rotation_value) <= 0.0) {
        return luaL_error(state, "transform.rotation must not be zero");
    }
    scene::Transform next = *current;
    if (position_status > 0) {
        next.position = glm::dvec3(position[0], position[1], position[2]);
    }
    if (rotation_status > 0) {
        next.rotation = glm::normalize(rotation_value);
    }
    if (scale_status > 0) {
        next.scale = glm::dvec3(scale[0], scale[1], scale[2]);
    }
    *current = next;
    return 0;
}

// ---- input ------------------------------------------------------------------------------------------------------------

// Reads the action name in argument 1 and resolves it. An unknown name raises an error.
platform::ActionId action_arg(lua_State* state) {
    const platform::ActionMap& actions = bound_actions(state);
    const char* name = luaL_checkstring(state, 1);
    const std::optional<platform::ActionId> id = actions.find(name);
    if (!id) {
        luaL_error(state, "unknown action '%s'", name);
    }
    return *id;
}

int input_value(lua_State* state) {
    const platform::ActionEvaluator& input = bound_input(state);
    lua_pushnumber(state, input.value(action_arg(state)));
    return 1;
}

int input_down(lua_State* state) {
    const platform::ActionEvaluator& input = bound_input(state);
    lua_pushboolean(state, input.down(action_arg(state)));
    return 1;
}

int input_pressed(lua_State* state) {
    const platform::ActionEvaluator& input = bound_input(state);
    lua_pushboolean(state, input.pressed(action_arg(state)));
    return 1;
}

int input_released(lua_State* state) {
    const platform::ActionEvaluator& input = bound_input(state);
    lua_pushboolean(state, input.released(action_arg(state)));
    return 1;
}

// ---- assets -----------------------------------------------------------------------------------------------------------

// Reads a GUID in text form. Text that is not a GUID raises an argument error.
core::Guid guid_arg(lua_State* state, int index) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, index, &length);
    const auto parsed = core::Guid::parse(std::string_view(text, length));
    if (!parsed.ok()) {
        luaL_argerror(state, index, "expected a GUID");
    }
    return parsed.value();
}

void push_text(lua_State* state, const std::string& text) {
    lua_pushlstring(state, text.data(), text.size());
}

int assets_find(lua_State* state) {
    const assets::AssetRegistry& registry = bound_assets(state);
    const char* text = luaL_checkstring(state, 1);
    const auto path = core::Path::from_utf8(text);
    const assets::AssetRecord* record = path.ok() ? registry.find_by_path(path.value()) : nullptr;
    if (record == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    push_text(state, record->guid.to_string());
    return 1;
}

int assets_exists(lua_State* state) {
    const assets::AssetRegistry& registry = bound_assets(state);
    lua_pushboolean(state, registry.find(guid_arg(state, 1)) != nullptr);
    return 1;
}

int assets_path(lua_State* state) {
    const assets::AssetRegistry& registry = bound_assets(state);
    const assets::AssetRecord* record = registry.find(guid_arg(state, 1));
    if (record == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    push_text(state, record->source_path.str());
    return 1;
}

int assets_importer(lua_State* state) {
    const assets::AssetRegistry& registry = bound_assets(state);
    const assets::AssetRecord* record = registry.find(guid_arg(state, 1));
    if (record == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    push_text(state, record->importer);
    return 1;
}

// The GUIDs the asset reaches through its dependencies, as an array. Nil for an asset that is not registered.
int assets_dependencies(lua_State* state) {
    const assets::AssetRegistry& registry = bound_assets(state);
    const core::Guid guid = guid_arg(state, 1);
    if (registry.find(guid) == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    const std::vector<core::Guid> closure = registry.dependency_closure(guid);
    lua_createtable(state, static_cast<int>(closure.size()), 0);
    for (std::size_t i = 0; i < closure.size(); ++i) {
        push_text(state, closure[i].to_string());
        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// ---- physics ----------------------------------------------------------------------------------------------------------

// Reads an optional filter table: layers (0..0xFFFFFFFF), include_triggers (boolean), and ignore (an entity). Absent or nil keeps the
// defaults. Every field is checked before the query runs.
physics::QueryFilter filter_arg(lua_State* state, int index) {
    physics::QueryFilter filter;
    if (lua_isnoneornil(state, index)) {
        return filter;
    }
    luaL_checktype(state, index, LUA_TTABLE);
    const int table = lua_absindex(state, index);

    lua_getfield(state, table, "layers");
    if (!lua_isnil(state, -1)) {
        int is_integer = 0;
        const lua_Integer layers = lua_tointegerx(state, -1, &is_integer);
        if (is_integer == 0 || layers < 0 || layers > 0xFFFFFFFFLL) {
            luaL_argerror(state, index, "filter.layers must be an integer from 0 to 0xFFFFFFFF");
        }
        filter.layers = static_cast<std::uint32_t>(layers);
    }
    lua_pop(state, 1);

    lua_getfield(state, table, "include_triggers");
    if (!lua_isnil(state, -1)) {
        if (!lua_isboolean(state, -1)) {
            luaL_argerror(state, index, "filter.include_triggers must be a boolean");
        }
        filter.include_triggers = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);

    lua_getfield(state, table, "ignore");
    if (!lua_isnil(state, -1)) {
        filter.ignore = entity_value(state, lua_gettop(state));
    }
    lua_pop(state, 1);
    return filter;
}

int physics_raycast(lua_State* state) {
    physics::PhysicsWorld& world = bound_physics(state);
    double origin[3] = {0.0, 0.0, 0.0};
    double direction[3] = {0.0, 0.0, 0.0};
    number_args(state, 1, origin, 3);
    number_args(state, 4, direction, 3);
    const double max_distance = luaL_checknumber(state, 7);
    if (!(max_distance > 0.0) || !std::isfinite(max_distance)) {
        return luaL_argerror(state, 7, "distance must be a positive number");
    }
    const glm::vec3 direction_value(direction[0], direction[1], direction[2]);
    if (glm::length(direction_value) <= 0.0f) {
        return luaL_argerror(state, 4, "direction must not be zero");
    }
    const physics::QueryFilter filter = filter_arg(state, 8);
    const auto hit = world.raycast(glm::dvec3(origin[0], origin[1], origin[2]), direction_value, static_cast<float>(max_distance), filter);
    if (!hit) {
        lua_pushnil(state);
        return 1;
    }
    const double position[3] = {hit->position.x, hit->position.y, hit->position.z};
    const double normal[3] = {hit->normal.x, hit->normal.y, hit->normal.z};
    lua_createtable(state, 0, 5);
    push_entity(state, hit->entity);
    lua_setfield(state, -2, "entity");
    push_numbers(state, position, 3);
    lua_setfield(state, -2, "position");
    push_numbers(state, normal, 3);
    lua_setfield(state, -2, "normal");
    lua_pushnumber(state, hit->distance);
    lua_setfield(state, -2, "distance");
    lua_pushboolean(state, hit->trigger);
    lua_setfield(state, -2, "trigger");
    return 1;
}

// Entities of the bodies that a sphere overlaps now, as an array.
int physics_overlap_sphere(lua_State* state) {
    physics::PhysicsWorld& world = bound_physics(state);
    double center[3] = {0.0, 0.0, 0.0};
    number_args(state, 1, center, 3);
    const double radius = luaL_checknumber(state, 4);
    if (!(radius > 0.0) || !std::isfinite(radius)) {
        return luaL_argerror(state, 4, "radius must be a positive number");
    }
    const physics::QueryFilter filter = filter_arg(state, 5);
    physics::BodyDesc shape;
    shape.shape = physics::ShapeKind::Sphere;
    shape.radius = static_cast<float>(radius);
    shape.position = glm::dvec3(center[0], center[1], center[2]);
    const std::vector<ecs::EntityHandle> found = world.overlaps(shape, filter);
    lua_createtable(state, static_cast<int>(found.size()), 0);
    for (std::size_t i = 0; i < found.size(); ++i) {
        push_entity(state, found[i]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// ---- events -----------------------------------------------------------------------------------------------------------

// Audio (US-057). Voices carry the script engine's owner tag, so the engine's destructor releases them. Every failure is returned to Lua
// through luaL_error after the C++ values are out of scope, because the error longjmps over this frame.
audio::AudioEngine& bound_audio(lua_State* state) {
    audio::AudioEngine* engine = engine_of(state).services().audio;
    if (engine == nullptr) {
        luaL_error(state, "no audio engine is bound");
    }
    return *engine;
}

// Reads an optional number field of an options table, with a default when the field is absent.
float option_number(lua_State* state, int table, const char* field, float fallback) {
    lua_getfield(state, table, field);
    float value = fallback;
    if (!lua_isnil(state, -1)) {
        if (lua_type(state, -1) != LUA_TNUMBER) {
            luaL_error(state, "option '%s' must be a number", field);
        }
        value = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    return value;
}

// Reads an optional boolean field of an options table, with a default when the field is absent.
bool option_flag(lua_State* state, int table, const char* field, bool fallback) {
    lua_getfield(state, table, field);
    const bool value = lua_isnil(state, -1) ? fallback : lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

// Reads the bus name of a table field, with a default when the field is absent. The name is compared while still on the Lua stack.
audio::Bus option_bus(lua_State* state, int table, audio::Bus fallback) {
    lua_getfield(state, table, "bus");
    audio::Bus bus = fallback;
    if (!lua_isnil(state, -1)) {
        const char* name = luaL_checkstring(state, -1);
        if (std::strcmp(name, "music") == 0) {
            bus = audio::Bus::Music;
        } else if (std::strcmp(name, "sfx") == 0) {
            bus = audio::Bus::Sfx;
        } else {
            luaL_error(state, "bus must be 'music' or 'sfx'");
        }
    }
    lua_pop(state, 1);
    return bus;
}

// The voice options table: bus, volume, pitch, loop, pan, min, max. Every field is optional.
audio::VoiceDesc voice_options(lua_State* state, int index, bool spatial) {
    audio::VoiceDesc desc;
    desc.spatial = spatial;
    if (lua_isnoneornil(state, index)) {
        return desc;
    }
    luaL_checktype(state, index, LUA_TTABLE);
    const int table = lua_absindex(state, index);
    desc.bus = option_bus(state, table, desc.bus);
    desc.volume = option_number(state, table, "volume", desc.volume);
    desc.pitch = option_number(state, table, "pitch", desc.pitch);
    desc.loop = option_flag(state, table, "loop", desc.loop);
    desc.pan = option_number(state, table, "pan", desc.pan);
    desc.min_distance = option_number(state, table, "min", desc.min_distance);
    desc.max_distance = option_number(state, table, "max", desc.max_distance);
    return desc;
}

// Starts a voice for the script and returns its id. The failure message is copied to the Lua stack before any C++ value is destroyed.
int push_voice(lua_State* state, audio::ClipId clip, audio::VoiceDesc desc) {
    audio::AudioEngine& engine = bound_audio(state);
    desc.owner = engine_of(state).audio_owner();
    {
        const auto voice = engine.play(clip, desc);
        if (voice.ok()) {
            lua_pushinteger(state, static_cast<lua_Integer>(voice.value()));
            return 1;
        }
        lua_pushstring(state, voice.error().message.c_str());
    }
    return luaL_error(state, "audio.play: %s", lua_tostring(state, -1));
}

int audio_load(lua_State* state) {
    audio::AudioEngine& engine = bound_audio(state);
    const char* text = luaL_checkstring(state, 1);
    {
        const auto path = core::Path::from_utf8(text);
        if (!path.ok()) {
            lua_pushliteral(state, "path is not valid UTF-8");
        } else {
            const auto clip = engine.load_clip(path.value());
            if (clip.ok()) {
                lua_pushinteger(state, static_cast<lua_Integer>(clip.value()));
                return 1;
            }
            lua_pushstring(state, clip.error().message.c_str());
        }
    }
    return luaL_error(state, "audio.load: %s", lua_tostring(state, -1));
}

int audio_play(lua_State* state) {
    const auto clip = static_cast<audio::ClipId>(luaL_checkinteger(state, 1));
    return push_voice(state, clip, voice_options(state, 2, false));
}

int audio_play_at(lua_State* state) {
    const auto clip = static_cast<audio::ClipId>(luaL_checkinteger(state, 1));
    double position[3] = {0.0, 0.0, 0.0};
    number_args(state, 2, position, 3);
    audio::VoiceDesc desc = voice_options(state, 5, true);
    desc.position = glm::dvec3(position[0], position[1], position[2]);
    return push_voice(state, clip, desc);
}

int audio_stop(lua_State* state) {
    const auto voice = static_cast<audio::VoiceId>(luaL_checkinteger(state, 1));
    lua_pushboolean(state, bound_audio(state).stop(voice) ? 1 : 0);
    return 1;
}

int audio_is_playing(lua_State* state) {
    const auto voice = static_cast<audio::VoiceId>(luaL_checkinteger(state, 1));
    lua_pushboolean(state, bound_audio(state).is_playing(voice) ? 1 : 0);
    return 1;
}

int audio_set_position(lua_State* state) {
    const auto voice = static_cast<audio::VoiceId>(luaL_checkinteger(state, 1));
    double position[3] = {0.0, 0.0, 0.0};
    number_args(state, 2, position, 3);
    lua_pushboolean(state, bound_audio(state).set_voice_position(voice, glm::dvec3(position[0], position[1], position[2])) ? 1 : 0);
    return 1;
}

// set_listener(px, py, pz, fx, fy, fz, ux, uy, uz): position, forward, and up in world space.
int audio_set_listener(lua_State* state) {
    double values[9] = {};
    number_args(state, 1, values, 9);
    audio::Listener listener;
    listener.position = glm::dvec3(values[0], values[1], values[2]);
    listener.forward = glm::vec3(static_cast<float>(values[3]), static_cast<float>(values[4]), static_cast<float>(values[5]));
    listener.up = glm::vec3(static_cast<float>(values[6]), static_cast<float>(values[7]), static_cast<float>(values[8]));
    bound_audio(state).set_listener(listener);
    return 0;
}

int audio_set_bus_volume(lua_State* state) {
    const char* name = luaL_checkstring(state, 1);
    const float volume = static_cast<float>(luaL_checknumber(state, 2));
    audio::Bus bus = audio::Bus::Master;
    if (std::strcmp(name, "master") == 0) {
        bus = audio::Bus::Master;
    } else if (std::strcmp(name, "music") == 0) {
        bus = audio::Bus::Music;
    } else if (std::strcmp(name, "sfx") == 0) {
        bus = audio::Bus::Sfx;
    } else {
        return luaL_argerror(state, 1, "bus must be 'master', 'music', or 'sfx'");
    }
    if (!std::isfinite(volume) || volume < 0.0f) {
        return luaL_argerror(state, 2, "volume must be a finite number, zero or more");
    }
    bound_audio(state).set_bus_volume(bus, volume);
    return 0;
}

int audio_pause(lua_State* state) {
    luaL_checkany(state, 1);
    bound_audio(state).set_paused(lua_toboolean(state, 1) != 0);
    return 0;
}

// play_music(path, options): streamed music. The options take loop (default true) and volume.
int audio_play_music(lua_State* state) {
    audio::AudioEngine& engine = bound_audio(state);
    const char* text = luaL_checkstring(state, 1);
    audio::MusicDesc desc;
    if (!lua_isnoneornil(state, 2)) {
        luaL_checktype(state, 2, LUA_TTABLE);
        const int table = lua_absindex(state, 2);
        desc.loop = option_flag(state, table, "loop", desc.loop);
        desc.volume = option_number(state, table, "volume", desc.volume);
    }
    {
        const auto path = core::Path::from_utf8(text);
        if (!path.ok()) {
            lua_pushliteral(state, "path is not valid UTF-8");
        } else {
            const auto played = engine.play_music_file(path.value(), desc);
            if (played.ok()) {
                return 0;
            }
            lua_pushstring(state, played.error().message.c_str());
        }
    }
    return luaL_error(state, "audio.play_music: %s", lua_tostring(state, -1));
}

int audio_stop_music(lua_State* state) {
    bound_audio(state).stop_music();
    return 0;
}

int events_on(lua_State* state) {
    const char* name = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    lua_pushvalue(state, 2);
    const int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    const std::uint64_t id = engine_of(state).events().subscribe(name, ref);
    lua_pushinteger(state, static_cast<lua_Integer>(id));
    return 1;
}

int events_off(lua_State* state) {
    const lua_Integer id = luaL_checkinteger(state, 1);
    const bool removed = id > 0 && engine_of(state).events().unsubscribe(static_cast<std::uint64_t>(id));
    lua_pushboolean(state, removed);
    return 1;
}

// Calls every handler of the event with the payload, in subscription order. A handler that raises is reported to the host, and the
// remaining handlers still run.
int events_emit(lua_State* state) {
    ScriptEngine& engine = engine_of(state);
    const char* name = luaL_checkstring(state, 1);
    lua_settop(state, 2);  // a missing payload is nil
    for (const EventBus::Handler handler : engine.events().handlers(name)) {
        if (!engine.events().subscribed(handler.id)) {
            continue;  // an earlier handler removed this one
        }
        lua_rawgeti(state, LUA_REGISTRYINDEX, handler.ref);
        lua_pushvalue(state, 2);
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            const char* text = lua_tostring(state, -1);
            engine.events().report(ScriptError{ScriptErrorKind::Runtime, text != nullptr ? text : "error object is not a string"});
            lua_pop(state, 1);
        }
    }
    return 0;
}

// ---- installation -----------------------------------------------------------------------------------------------------

constexpr luaL_Reg kEntityFunctions[] = {
    {"create", entity_create},
    {"destroy", entity_destroy},
    {"alive", entity_alive},
    {"has", entity_has},
    {"add", entity_add},
    {"remove", entity_remove},
    {nullptr, nullptr},
};

constexpr luaL_Reg kTransformFunctions[] = {
    {"get", transform_get},
    {"set", transform_set},
    {nullptr, nullptr},
};

constexpr luaL_Reg kInputFunctions[] = {
    {"value", input_value},
    {"down", input_down},
    {"pressed", input_pressed},
    {"released", input_released},
    {nullptr, nullptr},
};

constexpr luaL_Reg kAssetFunctions[] = {
    {"find", assets_find},
    {"exists", assets_exists},
    {"path", assets_path},
    {"importer", assets_importer},
    {"dependencies", assets_dependencies},
    {nullptr, nullptr},
};

constexpr luaL_Reg kPhysicsFunctions[] = {
    {"raycast", physics_raycast},
    {"overlap_sphere", physics_overlap_sphere},
    {nullptr, nullptr},
};

// Animation (US-069). Scripts change parameters of the named state machines in the bound AnimatorSet; the host steps them. A name that is not a
// machine, or a parameter of the wrong type, raises a Lua error, and nothing changes. Each function holds no owned value across a raise.
animation::AnimatorSet& animators_of(lua_State* state) {
    animation::AnimatorSet* set = engine_of(state).services().animators;
    if (set == nullptr) {
        luaL_error(state, "no animator set is bound");
    }
    return *set;
}

animation::StateMachine& machine_arg(lua_State* state, int index) {
    const char* name = luaL_checkstring(state, index);
    animation::StateMachine* machine = animators_of(state).find(name);
    if (machine == nullptr) {
        luaL_error(state, "no animator named '%s'", name);
    }
    return *machine;
}

int animation_set_float(lua_State* state) {
    animation::StateMachine& machine = machine_arg(state, 1);
    const char* param = luaL_checkstring(state, 2);
    const lua_Number value = luaL_checknumber(state, 3);
    luaL_argcheck(state, std::isfinite(value), 3, "value must be a finite number");
    if (!machine.set_float(param, static_cast<float>(value))) {
        luaL_error(state, "animator '%s' has no float parameter '%s'", lua_tostring(state, 1), param);
    }
    return 0;
}

int animation_set_bool(lua_State* state) {
    animation::StateMachine& machine = machine_arg(state, 1);
    const char* param = luaL_checkstring(state, 2);
    luaL_checktype(state, 3, LUA_TBOOLEAN);
    if (!machine.set_bool(param, lua_toboolean(state, 3) != 0)) {
        luaL_error(state, "animator '%s' has no bool parameter '%s'", lua_tostring(state, 1), param);
    }
    return 0;
}

int animation_trigger(lua_State* state) {
    animation::StateMachine& machine = machine_arg(state, 1);
    const char* param = luaL_checkstring(state, 2);
    if (!machine.set_trigger(param)) {
        luaL_error(state, "animator '%s' has no trigger parameter '%s'", lua_tostring(state, 1), param);
    }
    return 0;
}

int animation_get_float(lua_State* state) {
    animation::StateMachine& machine = machine_arg(state, 1);
    const char* param = luaL_checkstring(state, 2);
    const std::optional<float> value = machine.get_float(param);
    if (!value) {
        luaL_error(state, "animator '%s' has no float parameter '%s'", lua_tostring(state, 1), param);
    }
    lua_pushnumber(state, static_cast<lua_Number>(*value));
    return 1;
}

int animation_get_bool(lua_State* state) {
    animation::StateMachine& machine = machine_arg(state, 1);
    const char* param = luaL_checkstring(state, 2);
    const std::optional<bool> value = machine.get_bool(param);
    if (!value) {
        luaL_error(state, "animator '%s' has no bool parameter '%s'", lua_tostring(state, 1), param);
    }
    lua_pushboolean(state, *value ? 1 : 0);
    return 1;
}

int animation_get_trigger(lua_State* state) {
    animation::StateMachine& machine = machine_arg(state, 1);
    const char* param = luaL_checkstring(state, 2);
    const std::optional<bool> value = machine.get_trigger(param);
    if (!value) {
        luaL_error(state, "animator '%s' has no trigger parameter '%s'", lua_tostring(state, 1), param);
    }
    lua_pushboolean(state, *value ? 1 : 0);
    return 1;
}

int animation_state(lua_State* state) {
    const std::string_view name = machine_arg(state, 1).state_name();
    lua_pushlstring(state, name.data(), name.size());
    return 1;
}

int animation_blending(lua_State* state) {
    lua_pushboolean(state, machine_arg(state, 1).blending() ? 1 : 0);
    return 1;
}

constexpr luaL_Reg kAnimationFunctions[] = {
    {"set_float", animation_set_float},
    {"set_bool", animation_set_bool},
    {"trigger", animation_trigger},
    {"get_float", animation_get_float},
    {"get_bool", animation_get_bool},
    {"get_trigger", animation_get_trigger},
    {"state", animation_state},
    {"blending", animation_blending},
    {nullptr, nullptr},
};

constexpr luaL_Reg kEventFunctions[] = {
    {"on", events_on},
    {"off", events_off},
    {"emit", events_emit},
    {nullptr, nullptr},
};

constexpr luaL_Reg kAudioFunctions[] = {
    {"load", audio_load},
    {"play", audio_play},
    {"play_at", audio_play_at},
    {"stop", audio_stop},
    {"is_playing", audio_is_playing},
    {"set_position", audio_set_position},
    {"set_listener", audio_set_listener},
    {"set_bus_volume", audio_set_bus_volume},
    {"pause", audio_pause},
    {"play_music", audio_play_music},
    {"stop_music", audio_stop_music},
    {nullptr, nullptr},
};

// Creates a global table of functions, each closing over the engine.
void open_module(lua_State* state, ScriptEngine& engine, const char* name, const luaL_Reg* functions) {
    lua_newtable(state);
    lua_pushlightuserdata(state, &engine);
    luaL_setfuncs(state, functions, 1);
    lua_setglobal(state, name);
}

}  // namespace

// Entity values. The userdata holds the handle itself, never a pointer into the registry.
void push_entity(lua_State* state, ecs::EntityHandle handle) {
    auto* slot = static_cast<ecs::EntityHandle*>(lua_newuserdatauv(state, sizeof(ecs::EntityHandle), 0));
    *slot = handle;  // trivially copyable, so the assignment starts the object's lifetime
    luaL_setmetatable(state, kEntityMeta);
}

void install_bindings(lua_State* state, ScriptEngine& engine) {
    luaL_newmetatable(state, kEntityMeta);
    lua_pushcfunction(state, entity_equal);
    lua_setfield(state, -2, "__eq");
    lua_pushcfunction(state, entity_tostring);
    lua_setfield(state, -2, "__tostring");
    lua_pop(state, 1);

    open_module(state, engine, "entity", kEntityFunctions);
    open_module(state, engine, "transform", kTransformFunctions);
    open_module(state, engine, "input", kInputFunctions);
    open_module(state, engine, "assets", kAssetFunctions);
    open_module(state, engine, "physics", kPhysicsFunctions);
    open_module(state, engine, "events", kEventFunctions);
    open_module(state, engine, "audio", kAudioFunctions);
    open_module(state, engine, "animation", kAnimationFunctions);
}

void ScriptEngine::set_global_entity(std::string_view name, ecs::EntityHandle entity) {
    const std::string global(name);
    push_entity(state_, entity);
    lua_setglobal(state_, global.c_str());
}

}  // namespace engine::script
