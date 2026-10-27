#include "engine/script/event_bus.hpp"

#include <algorithm>
#include <utility>

extern "C" {
#include "lauxlib.h"
}

namespace engine::script {

std::uint64_t EventBus::subscribe(std::string_view name, int ref) {
    const std::uint64_t id = next_id_++;
    subscriptions_.push_back(Subscription{std::string(name), Handler{id, ref}, owner_});
    return id;
}

bool EventBus::unsubscribe(std::uint64_t id) {
    const auto found = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                                    [id](const Subscription& entry) { return entry.handler.id == id; });
    if (found == subscriptions_.end()) {
        return false;
    }
    luaL_unref(state_, LUA_REGISTRYINDEX, found->handler.ref);
    subscriptions_.erase(found);
    return true;
}

std::uint64_t EventBus::set_owner(std::uint64_t owner) noexcept {
    return std::exchange(owner_, owner);
}

void EventBus::release_owner(std::uint64_t owner) {
    if (owner == 0) {
        return;
    }
    for (std::size_t i = 0; i < subscriptions_.size();) {
        if (subscriptions_[i].owner != owner) {
            ++i;
            continue;
        }
        luaL_unref(state_, LUA_REGISTRYINDEX, subscriptions_[i].handler.ref);
        subscriptions_.erase(subscriptions_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

bool EventBus::subscribed(std::uint64_t id) const {
    return std::any_of(subscriptions_.begin(), subscriptions_.end(),
                       [id](const Subscription& entry) { return entry.handler.id == id; });
}

std::vector<EventBus::Handler> EventBus::handlers(std::string_view name) const {
    std::vector<Handler> result;
    for (const Subscription& entry : subscriptions_) {
        if (entry.name == name) {
            result.push_back(entry.handler);
        }
    }
    return result;
}

void EventBus::report(ScriptError error) {
    errors_.push_back(std::move(error));
}

std::vector<ScriptError> EventBus::take_errors() {
    return std::exchange(errors_, {});
}

}  // namespace engine::script
