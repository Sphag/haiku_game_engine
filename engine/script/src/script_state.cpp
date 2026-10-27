#include "script_state.hpp"

#include "engine/core/utf8.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
#include "lauxlib.h"
}

namespace engine::script::detail {
namespace {

constexpr int kMaxDepth = 32;

// The reason a value cannot be saved, or nothing when it can.
using Problem = std::optional<std::string>;
using Key = std::variant<std::int64_t, std::string>;

// Only these types are state. Functions, userdata, and threads are skipped by the table walk.
bool is_state_type(int type) {
    return type == LUA_TBOOLEAN || type == LUA_TNUMBER || type == LUA_TSTRING || type == LUA_TTABLE;
}

Problem encode_value(lua_State* state, nlohmann::json& out, int depth, std::vector<const void*>& path);

// Encodes the table at the absolute stack index `index`. Keys 1..n make a JSON array and string keys make a JSON object. The
// environment is always an object (`force_object`), so its keys are names.
Problem encode_table(lua_State* state, int index, bool force_object, nlohmann::json& out, int depth, std::vector<const void*>& path) {
    if (depth > kMaxDepth) {
        return std::string("a table is nested more than 32 levels deep");
    }
    const void* identity = lua_topointer(state, index);
    if (std::find(path.begin(), path.end(), identity) != path.end()) {
        return std::string("a table refers to itself");
    }
    path.push_back(identity);

    std::vector<std::pair<Key, nlohmann::json>> entries;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (!is_state_type(lua_type(state, -1))) {
            lua_pop(state, 1);
            continue;
        }
        Key key;
        if (lua_type(state, -2) == LUA_TSTRING) {
            size_t length = 0;
            const char* text = lua_tolstring(state, -2, &length);
            std::string name(text, length);
            if (!core::is_valid_utf8(name)) {
                return std::string("a table key is not valid UTF-8");
            }
            key = std::move(name);
        } else if (lua_isinteger(state, -2)) {
            key = static_cast<std::int64_t>(lua_tointeger(state, -2));
        } else {
            return std::string("a table key must be a string or an integer");
        }
        nlohmann::json value;
        if (auto problem = encode_value(state, value, depth + 1, path)) {
            return problem;
        }
        entries.emplace_back(std::move(key), std::move(value));
        lua_pop(state, 1);
    }
    path.pop_back();

    const bool all_strings = std::all_of(entries.begin(), entries.end(), [](const auto& entry) {
        return std::holds_alternative<std::string>(entry.first);
    });
    if (force_object || all_strings || entries.empty()) {
        if (!all_strings && !entries.empty()) {
            return std::string("a global must have a string name");
        }
        out = nlohmann::json::object();
        for (auto& entry : entries) {
            out[std::get<std::string>(entry.first)] = std::move(entry.second);
        }
        return std::nullopt;
    }

    const bool all_integers = std::all_of(entries.begin(), entries.end(), [](const auto& entry) {
        return std::holds_alternative<std::int64_t>(entry.first);
    });
    if (!all_integers) {
        return std::string("a table must have only string keys, or only the keys 1 to n");
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return std::get<std::int64_t>(a.first) < std::get<std::int64_t>(b.first);
    });
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (std::get<std::int64_t>(entries[i].first) != static_cast<std::int64_t>(i) + 1) {
            return std::string("a table must have only string keys, or only the keys 1 to n");
        }
    }
    out = nlohmann::json::array();
    for (auto& entry : entries) {
        out.push_back(std::move(entry.second));
    }
    return std::nullopt;
}

// Encodes the value on the top of the stack. Integers stay integers and floats stay floats, so the JSON type round-trips.
Problem encode_value(lua_State* state, nlohmann::json& out, int depth, std::vector<const void*>& path) {
    switch (lua_type(state, -1)) {
        case LUA_TBOOLEAN:
            out = lua_toboolean(state, -1) != 0;
            return std::nullopt;
        case LUA_TNUMBER:
            if (lua_isinteger(state, -1)) {
                out = static_cast<std::int64_t>(lua_tointeger(state, -1));
                return std::nullopt;
            }
            {
                const double value = lua_tonumber(state, -1);
                if (!std::isfinite(value)) {
                    return std::string("a non-finite number cannot be saved");
                }
                out = value;
            }
            return std::nullopt;
        case LUA_TSTRING: {
            size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            std::string value(text, length);
            if (!core::is_valid_utf8(value)) {
                return std::string("a string is not valid UTF-8");
            }
            out = std::move(value);
            return std::nullopt;
        }
        case LUA_TTABLE:
            return encode_table(state, lua_gettop(state), false, out, depth, path);
        default:
            return std::string("a value of this type cannot be saved");
    }
}

// Pushes the Lua value for a JSON value. On failure the caller discards the stack above its base.
Problem push_json(lua_State* state, const nlohmann::json& value, int depth) {
    if (depth > kMaxDepth) {
        return std::string("a table is nested more than 32 levels deep");
    }
    switch (value.type()) {
        case nlohmann::json::value_t::boolean:
            lua_pushboolean(state, value.get<bool>() ? 1 : 0);
            return std::nullopt;
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            lua_pushinteger(state, static_cast<lua_Integer>(value.get<std::int64_t>()));
            return std::nullopt;
        case nlohmann::json::value_t::number_float:
            lua_pushnumber(state, value.get<double>());
            return std::nullopt;
        case nlohmann::json::value_t::string: {
            const std::string& text = value.get_ref<const std::string&>();
            lua_pushlstring(state, text.data(), text.size());
            return std::nullopt;
        }
        case nlohmann::json::value_t::array: {
            lua_createtable(state, static_cast<int>(value.size()), 0);
            lua_Integer index = 1;
            for (const auto& item : value) {
                if (auto problem = push_json(state, item, depth + 1)) {
                    return problem;
                }
                lua_rawseti(state, -2, index);
                ++index;
            }
            return std::nullopt;
        }
        case nlohmann::json::value_t::object: {
            lua_createtable(state, 0, static_cast<int>(value.size()));
            for (const auto& [name, item] : value.items()) {
                lua_pushlstring(state, name.data(), name.size());
                if (auto problem = push_json(state, item, depth + 1)) {
                    return problem;
                }
                lua_rawset(state, -3);
            }
            return std::nullopt;
        }
        default:
            return std::string("the state holds a null value");
    }
}

}  // namespace

core::Result<std::string, ScriptError> encode_environment(lua_State* state, int env_ref) {
    const int base = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, env_ref);
    nlohmann::json out;
    std::vector<const void*> path;
    const Problem problem = encode_table(state, lua_gettop(state), true, out, 0, path);
    lua_settop(state, base);
    if (problem) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, "capture_state: " + *problem});
    }
    return core::success(out.dump());
}

core::Result<core::Unit, ScriptError> decode_environment(lua_State* state, int env_ref, std::string_view text) {
    const int base = lua_gettop(state);
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return core::failure(ScriptError{ScriptErrorKind::Runtime, "restore_state: the state is not a JSON object"});
    }

    // Stage the whole object first, so a bad value leaves the environment as it was.
    lua_newtable(state);
    const int stage = lua_gettop(state);
    for (const auto& [name, value] : document.items()) {
        lua_pushlstring(state, name.data(), name.size());
        if (auto problem = push_json(state, value, 1)) {
            lua_settop(state, base);
            return core::failure(ScriptError{ScriptErrorKind::Runtime, "restore_state: " + *problem});
        }
        lua_rawset(state, stage);
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, env_ref);
    const int env = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, stage) != 0) {
        // The key and value are on the stack; copy them so lua_rawset can pop the copies and the iteration key stays on top.
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_rawset(state, env);
        lua_pop(state, 1);
    }
    lua_settop(state, base);
    return core::success();
}

}  // namespace engine::script::detail
