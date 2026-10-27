// Source watching (US-025): the Linux backend, on inotify. inotify watches one directory at a time, so the backend adds a
// watch for every directory in the tree and for each directory created or moved in later.

#include "asset_converter/watch.hpp"

#include "engine/core/path.hpp"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>

namespace engine::converter {

namespace {

namespace fs = std::filesystem;

// Close-write, not modify: a file is converted once its writer has closed it, not while a save is still in progress.
constexpr std::uint32_t kWatchMask = IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                                     IN_MOVE_SELF;

std::string utf8_text(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string join(const std::string& directory, const std::string& name) {
    return directory.empty() ? name : directory + "/" + name;
}

class InotifyWatch final : public ChangeSource {
public:
    InotifyWatch(int fd, fs::path root) : fd_(fd), root_(std::move(root)) {}

    ~InotifyWatch() override {
        ::close(fd_);
    }

    // Watches `absolute` (project-relative `relative`) and every directory below it. A directory the OS refuses fails the
    // whole call, which the caller reports.
    bool add_tree(const fs::path& absolute, const std::string& relative, std::string& error) {
        const int wd = ::inotify_add_watch(fd_, absolute.c_str(), kWatchMask);
        if (wd < 0) {
            error = "cannot watch " + utf8_text(absolute) + ": " + std::strerror(errno);
            return false;
        }
        // inotify returns the same descriptor for a directory it already watches, so a moved directory gets its new name.
        dirs_[wd] = relative;

        std::error_code ec;
        for (fs::directory_iterator it(absolute, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_ec;
            const fs::file_status status = it->symlink_status(type_ec);
            if (type_ec || !fs::is_directory(status)) {
                continue;
            }
            if (!add_tree(it->path(), join(relative, utf8_text(it->path().filename())), error)) {
                return false;
            }
        }
        return !ec;
    }

    WatchEvents poll(std::chrono::milliseconds timeout) override {
        WatchEvents result;
        pollfd descriptor{fd_, POLLIN, 0};
        // An interrupted wait (EINTR) reads as a timeout, and the caller simply waits again.
        if (::poll(&descriptor, 1, static_cast<int>(timeout.count())) <= 0) {
            return result;
        }

        // One read per wait. The descriptor is blocking, so a second read would wait for more events. Events that do not fit
        // stay queued and arrive on the next wait.
        alignas(inotify_event) char buffer[16 * 1024];
        const ssize_t bytes = ::read(fd_, buffer, sizeof buffer);
        if (bytes <= 0) {
            return result;
        }

        for (char* cursor = buffer; cursor < buffer + bytes;) {
            const auto* event = reinterpret_cast<const inotify_event*>(cursor);
            cursor += sizeof(inotify_event) + event->len;

            if ((event->mask & IN_Q_OVERFLOW) != 0) {
                result.overflow = true;
                continue;
            }
            if ((event->mask & IN_IGNORED) != 0) {
                dirs_.erase(event->wd);  // the directory was removed; its watch is gone with it
                continue;
            }
            // A watched directory itself was deleted or moved. Its children are not reported individually, so rescan.
            if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                result.overflow = true;
                continue;
            }

            const auto directory = dirs_.find(event->wd);
            if (directory == dirs_.end()) {
                continue;
            }
            const std::string name(event->name, ::strnlen(event->name, event->len));
            if (name.empty()) {
                continue;
            }
            const std::string relative = join(directory->second, name);
            result.paths.push_back(relative);

            // A new directory may already hold files that no event reports, so its whole tree is watched and rescanned.
            if ((event->mask & IN_ISDIR) != 0 && (event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                std::string ignored;
                if (!add_tree(root_ / relative, relative, ignored)) {
                    result.overflow = true;
                }
            }
        }
        return result;
    }

private:
    int fd_;
    fs::path root_;
    std::map<int, std::string> dirs_;  // watch descriptor -> project-relative directory
};

}  // namespace

core::Result<std::unique_ptr<ChangeSource>, std::string> open_directory_watch(const core::Path& root) {
    const int fd = ::inotify_init1(IN_CLOEXEC);
    if (fd < 0) {
        return core::failure(std::string("cannot create an inotify instance: ") + std::strerror(errno));
    }
    auto watch = std::make_unique<InotifyWatch>(fd, core::to_native_path(root));
    std::string error;
    if (!watch->add_tree(core::to_native_path(root), "", error)) {
        return core::failure(error);
    }
    return core::success(std::unique_ptr<ChangeSource>(std::move(watch)));
}

}  // namespace engine::converter
