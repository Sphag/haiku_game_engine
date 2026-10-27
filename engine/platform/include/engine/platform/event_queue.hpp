#pragma once

#include "engine/platform/window.hpp"

#include <vector>

namespace engine::platform {

// FIFO of window events. A resize that follows another resize of the same window replaces it, so a drag that
// produces hundreds of size messages leaves one event with the latest size. Every other event is kept in order.
class EventQueue {
public:
    void push(WindowEvent event);

    // Returns the queued events in order and empties the queue.
    [[nodiscard]] std::vector<WindowEvent> drain();

    [[nodiscard]] bool empty() const noexcept {
        return events_.empty();
    }

private:
    std::vector<WindowEvent> events_;
};

}  // namespace engine::platform
