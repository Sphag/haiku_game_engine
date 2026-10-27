#include "gamepads_linux.hpp"

#include "engine/core/log.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace engine::platform::linux_input {

namespace {

constexpr auto kRescanInterval = std::chrono::milliseconds(500);
constexpr const char* kInputDirectory = "/dev/input";
constexpr std::size_t kKeyBitBytes = evdev::kKeyMax / 8 + 1;
constexpr std::size_t kAbsBitBytes = evdev::kAbsMax / 8 + 1;

// struct input_absinfo from linux/input.h: value, minimum, maximum, fuzz, flat, resolution.
struct AbsInfo {
    std::int32_t value;
    std::int32_t minimum;
    std::int32_t maximum;
    std::int32_t fuzz;
    std::int32_t flat;
    std::int32_t resolution;
};

// The evdev ioctl numbers from linux/input.h, written out so no kernel header is needed: direction (read, 2) in the top
// bits, then the size, the type 'E', and the number. EVIOCGBIT(ev, len) reads a bitmap of events; EVIOCGABS(axis)
// reads one axis's AbsInfo.
constexpr unsigned long kIocRead = 2UL << 30;
constexpr unsigned long eviocgbit(unsigned long event_type, std::size_t length) {
    return kIocRead | (static_cast<unsigned long>(length) << 16) | (static_cast<unsigned long>('E') << 8) |
           (0x20UL + event_type);
}
constexpr unsigned long eviocgabs(unsigned long axis) {
    return kIocRead | (static_cast<unsigned long>(sizeof(AbsInfo)) << 16) | (static_cast<unsigned long>('E') << 8) |
           (0x40UL + axis);
}

bool has_bit(const std::array<std::byte, kAbsBitBytes>& bits, std::uint16_t code) {
    return (std::to_integer<unsigned>(bits[code / 8]) >> (code % 8)) & 1U;
}

}  // namespace

GamepadReader::~GamepadReader() {
    for (const Device& device : devices_) {
        ::close(device.fd);
    }
}

void GamepadReader::poll() {
    const auto now = std::chrono::steady_clock::now();
    if (!scanned_ || now - last_scan_ >= kRescanInterval) {
        scan();
        last_scan_ = now;
        scanned_ = true;
    }
    for (std::size_t i = 0; i < devices_.size();) {
        bool lost = false;
        read_device(devices_[i], lost);
        if (lost) {
            ::close(devices_[i].fd);
            devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

GamepadSnapshot GamepadReader::snapshot(std::uint32_t slot) const {
    for (const Device& device : devices_) {
        if (device.slot == slot) {
            return device.decoder.snapshot();
        }
    }
    return GamepadSnapshot{};
}

void GamepadReader::scan() {
    DIR* directory = ::opendir(kInputDirectory);
    if (directory == nullptr) {
        const auto failure = errno == EACCES ? evdev::OpenFailure::PermissionDenied : evdev::OpenFailure::Missing;
        report_once(kInputDirectory, evdev::describe_open_failure(kInputDirectory, failure));
        return;
    }
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.rfind("event", 0) != 0) {
            continue;
        }
        const std::string path = std::string(kInputDirectory) + "/" + name;
        if (rejected_.count(path) != 0) {
            continue;
        }
        bool open_already = false;
        for (const Device& device : devices_) {
            open_already = open_already || device.path == path;
        }
        if (!open_already) {
            open_candidate(path);
        }
    }
    ::closedir(directory);
}

void GamepadReader::open_candidate(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const auto failure = errno == EACCES   ? evdev::OpenFailure::PermissionDenied
                             : errno == ENOENT ? evdev::OpenFailure::Missing
                                               : evdev::OpenFailure::Other;
        report_once(path, evdev::describe_open_failure(path, failure));
        return;
    }

    std::array<std::byte, kKeyBitBytes> key_bits{};
    std::array<std::byte, kAbsBitBytes> abs_bits{};
    const bool read_keys = ::ioctl(fd, eviocgbit(evdev::kEvKey, kKeyBitBytes), key_bits.data()) >= 0;
    const bool read_abs = ::ioctl(fd, eviocgbit(evdev::kEvAbs, kAbsBitBytes), abs_bits.data()) >= 0;
    if (!read_keys || !read_abs ||
        !evdev::is_gamepad(key_bits.data(), key_bits.size(), abs_bits.data(), abs_bits.size())) {
        ::close(fd);
        rejected_.insert(path);
        return;
    }

    const auto slot = evdev::free_slot([this] {
        std::array<bool, kMaxGamepads> in_use{};
        for (const Device& device : devices_) {
            in_use[device.slot] = true;
        }
        return in_use;
    }());
    if (!slot) {
        ::close(fd);
        report_once(path, "ignoring " + path + ": all " + std::to_string(kMaxGamepads) + " gamepad slots are in use");
        return;
    }

    Device device;
    device.path = path;
    device.fd = fd;
    device.slot = *slot;
    // Ranges come from the device, so each stick and trigger normalizes against its own limits.
    for (std::uint16_t code = 0; code <= evdev::kAbsMax; ++code) {
        if (!has_bit(abs_bits, code) || !evdev::axis_from_code(code)) {
            continue;
        }
        AbsInfo info{};
        if (::ioctl(fd, eviocgabs(code), &info) >= 0) {
            device.decoder.set_range(code, evdev::AxisRange{info.minimum, info.maximum, info.flat});
        }
    }
    devices_.push_back(std::move(device));
    core::log_message(core::LogLevel::Info, "platform", "gamepad connected: " + path);
}

void GamepadReader::read_device(Device& device, bool& lost) {
    std::array<std::byte, evdev::kRecordSize * 32> buffer{};
    for (;;) {
        const ssize_t count = ::read(device.fd, buffer.data(), buffer.size());
        if (count > 0) {
            const auto size = static_cast<std::size_t>(count);
            for (const evdev::Event& event : evdev::decode_records(buffer.data(), size)) {
                device.decoder.apply(event);
            }
            continue;
        }
        if (count < 0 && errno == EAGAIN) {
            return;  // nothing new this frame
        }
        // End of file or ENODEV: the device was unplugged. Its slot reads as disconnected from now on.
        lost = true;
        core::log_message(core::LogLevel::Info, "platform", "gamepad disconnected: " + device.path);
        return;
    }
}

void GamepadReader::report_once(const std::string& path, const std::string& message) {
    if (reported_.insert(path).second) {
        core::log_message(core::LogLevel::Warning, "platform", message);
    }
}

}  // namespace engine::platform::linux_input
