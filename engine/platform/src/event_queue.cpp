#include "engine/platform/event_queue.hpp"

#include <utility>

namespace engine::platform {

void EventQueue::push(WindowEvent event) {
    if (!events_.empty() && std::holds_alternative<WindowResized>(event.data)) {
        WindowEvent& last = events_.back();
        if (last.window == event.window && std::holds_alternative<WindowResized>(last.data)) {
            last.data = std::move(event.data);
            return;
        }
    }
    events_.push_back(std::move(event));
}

std::vector<WindowEvent> EventQueue::drain() {
    std::vector<WindowEvent> drained;
    drained.swap(events_);
    return drained;
}

}  // namespace engine::platform
