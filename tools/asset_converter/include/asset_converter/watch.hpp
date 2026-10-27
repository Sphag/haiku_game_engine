#pragma once

// Source watching (US-025): the converter converts a project again when its sources change, so an editor or a game
// creator sees the result without a manual command.
//
// Passes. A session runs one cook pass at start. After that, a change notification starts a pass only when the project's
// content changed. The session compares a snapshot of every source (path and content hash) with the snapshot of the last
// pass. Equal snapshots mean nothing a pass reads has changed, so no pass runs. This is what stops a rebuild loop: a pass
// writes only into the output directory, and touching a file without changing its bytes does not start another pass.
// Changes inside the output directory are dropped before they reach the debouncer, so a project that contains its output
// does not feed its own writes back in.
//
// Debouncing. A burst of notifications becomes one pass. A pass starts after `quiet` without a notification, or once the
// first pending notification has waited `max_wait`, so a steady stream of saves still produces passes.
//
// Incremental work. A pass is a normal cook. The cache in converter.cpp reuses every import and cook whose keys did not
// change, so only the changed assets and their dependents are converted again. A renamed or deleted dependency blocks its
// dependents with a diagnostic. Their previous cooked files stay in place, so the runtime keeps the last working version.
//
// Platforms. open_directory_watch() uses ReadDirectoryChangesW on Windows and inotify on Linux. Both report paths relative
// to the project root. A lost-event overflow is reported as a change, so the next pass rescans the whole project.

#include "asset_converter/converter.hpp"
#include "asset_converter/importer.hpp"

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine::converter {

// Notifications since the last poll. Paths are project-relative with '/' separators.
struct WatchEvents {
    std::vector<std::string> paths;
    bool overflow = false;  // the OS dropped notifications; the next pass rescans everything
};

// Where change notifications come from. The OS backends implement it; tests script it.
class ChangeSource {
public:
    ChangeSource() = default;
    virtual ~ChangeSource() = default;
    ChangeSource(const ChangeSource&) = delete;
    ChangeSource& operator=(const ChangeSource&) = delete;

    // Waits up to `timeout` for notifications and returns the ones that arrived. An empty result means the wait timed out.
    [[nodiscard]] virtual WatchEvents poll(std::chrono::milliseconds timeout) = 0;
};

// Watches the whole tree under `root`, including directories created after the watch started.
[[nodiscard]] core::Result<std::unique_ptr<ChangeSource>, std::string> open_directory_watch(const core::Path& root);

struct WatchOptions {
    std::chrono::milliseconds quiet{300};      // a pass starts after this long without a notification
    std::chrono::milliseconds max_wait{2000};  // a pass starts once the oldest pending notification has waited this long
};

// Turns a stream of notifications into due passes. It holds no clock: the caller passes the time, so the rules are
// testable without sleeping.
class Debouncer {
public:
    using Clock = std::chrono::steady_clock;

    explicit Debouncer(WatchOptions options) noexcept : options_(options) {}

    // A notification arrived at `now`.
    void note(Clock::time_point now) noexcept;

    [[nodiscard]] bool pending() const noexcept {
        return pending_;
    }

    // True when a notification is pending and either the quiet window or the maximum wait has elapsed.
    [[nodiscard]] bool due(Clock::time_point now) const noexcept;

    // How long until the next pass becomes due. Empty when nothing is pending.
    [[nodiscard]] std::optional<Clock::duration> until_due(Clock::time_point now) const noexcept;

    void clear() noexcept {
        pending_ = false;
    }

private:
    WatchOptions options_;
    bool pending_ = false;
    Clock::time_point first_{};
    Clock::time_point last_{};
};

// Relative path of every regular file under `project` (sorted), mapped to the SHA-256 of its bytes. A file that cannot be
// read is left out, and the next notification for it starts another pass. Files under `skip` are not listed.
using SourceSnapshot = std::map<std::string, std::string>;
[[nodiscard]] core::Result<SourceSnapshot, std::string> take_snapshot(const core::Path& project,
                                                                      const std::optional<core::Path>& skip);

// A watch over one project. start() runs the first pass. step() waits for notifications and runs a pass when one is due
// and the project changed. The session is driven by one thread.
class WatchSession {
public:
    // The session owns its importers, so a registry built for the session can be a temporary.
    WatchSession(Options options, ImporterRegistry importers, ChangeSource& source, WatchOptions watch = {});

    // Runs the first conversion of the project and remembers its snapshot.
    [[nodiscard]] core::Result<Report, std::string> start();

    // Waits up to `wait` for notifications. Returns the report of a pass when one ran, and an empty optional otherwise.
    // A usage error (for example, no output directory) fails with a message.
    [[nodiscard]] core::Result<std::optional<Report>, std::string> step(std::chrono::milliseconds wait);

private:
    [[nodiscard]] bool inside_output(const std::string& relative) const;
    [[nodiscard]] core::Result<Report, std::string> convert();

    Options options_;
    ImporterRegistry importers_;
    ChangeSource& source_;
    Debouncer debouncer_;
    std::optional<std::string> output_relative_;  // the output directory relative to the project, when it is inside it
    std::optional<SourceSnapshot> last_snapshot_;
};

}  // namespace engine::converter
