#pragma once

// Linux gamepads through evdev (US-010). Opens /dev/input/event* nodes that describe a gamepad, reads their records,
// and rescans for hotplug. The decoding is platform-neutral (engine/platform/evdev.hpp); this file only does the file
// and ioctl work. Nothing here calls a ready-made input library.
#include "engine/platform/evdev.hpp"
#include "engine/platform/input_mapping.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace engine::platform::linux_input {

class GamepadReader {
public:
    GamepadReader() = default;
    ~GamepadReader();

    GamepadReader(const GamepadReader&) = delete;
    GamepadReader& operator=(const GamepadReader&) = delete;

    // Rescans for devices when the rescan interval has passed, then reads every open device. Failures to open a node
    // are logged once per path, with the fix for them (see evdev::describe_open_failure).
    void poll();

    // State of one slot at the last poll. An empty slot is a disconnected snapshot.
    [[nodiscard]] GamepadSnapshot snapshot(std::uint32_t slot) const;

private:
    struct Device {
        std::string path;
        int fd = -1;
        std::uint32_t slot = 0;
        evdev::PadDecoder decoder;
    };

    void scan();
    void open_candidate(const std::string& path);
    void read_device(Device& device, bool& lost);
    void report_once(const std::string& path, const std::string& message);

    std::vector<Device> devices_;
    std::set<std::string> rejected_;  // nodes that are not gamepads
    std::set<std::string> reported_;  // open failures already logged
    std::chrono::steady_clock::time_point last_scan_{};
    bool scanned_ = false;
};

}  // namespace engine::platform::linux_input
