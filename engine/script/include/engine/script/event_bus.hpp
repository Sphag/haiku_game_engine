#pragma once

// Script events (US-055). Scripts subscribe a Lua function to a named event and emit events with a payload. The bus keeps each
// handler as a reference in the Lua registry, so the function lives until it unsubscribes or the state closes. A handler that
// raises an error does not stop the others: the error is reported here and taken by the host. No Lua type appears in this header.
//
// Each subscription carries an owner tag (US-056). The script runner sets the owner while an instance's code runs, so every
// subscription the instance makes is released with it, and release_owner() drops them all at teardown.

#include "engine/script/script_error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace engine::script {

class EventBus {
public:
    struct Handler {
        std::uint64_t id = 0;
        int ref = 0;  // Lua registry reference to the handler function
    };

    explicit EventBus(lua_State* state) noexcept : state_(state) {}

    // Stores a handler the caller has already referenced with luaL_ref. Ids start at 1 and are never reused. The new
    // subscription belongs to the current owner.
    [[nodiscard]] std::uint64_t subscribe(std::string_view name, int ref);

    // Drops a handler and releases its reference. Returns false for an unknown or already removed id.
    bool unsubscribe(std::uint64_t id);

    // Makes `owner` the owner of later subscriptions (0 means none). Returns the previous owner so a scope can restore it.
    std::uint64_t set_owner(std::uint64_t owner) noexcept;

    // Drops every subscription that `owner` made, and releases their references. Owner 0 is never released.
    void release_owner(std::uint64_t owner);

    [[nodiscard]] bool subscribed(std::uint64_t id) const;

    // Number of live subscriptions across all events.
    [[nodiscard]] std::size_t size() const noexcept {
        return subscriptions_.size();
    }

    // The handlers of one event in subscription order. The copy lets a handler subscribe or unsubscribe while the event runs.
    [[nodiscard]] std::vector<Handler> handlers(std::string_view name) const;

    void report(ScriptError error);
    [[nodiscard]] std::vector<ScriptError> take_errors();

private:
    struct Subscription {
        std::string name;
        Handler handler;
        std::uint64_t owner = 0;
    };

    lua_State* state_ = nullptr;
    std::uint64_t next_id_ = 1;
    std::uint64_t owner_ = 0;
    std::vector<Subscription> subscriptions_;
    std::vector<ScriptError> errors_;
};

}  // namespace engine::script
