// Source watching (US-025): the portable half. The OS notification backends are in watch_linux.cpp and watch_win32.cpp.

#include "asset_converter/watch.hpp"

#include "engine/assets/content_hash.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace engine::converter {

namespace {

namespace fs = std::filesystem;
using Clock = Debouncer::Clock;

// The path below `root`, or nothing when `path` is not under it. Both are normalized paths.
std::optional<std::string> relative_under(const std::string& root, const std::string& path) {
    const std::string prefix = (!root.empty() && root.back() == '/') ? root : root + "/";
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    return path.substr(prefix.size());
}

// True when `relative` is `directory` or lies below it. Both are project-relative.
bool is_within(const std::string& directory, const std::string& relative) {
    return relative == directory || (relative.size() > directory.size() && relative.compare(0, directory.size(), directory) == 0 &&
                                     relative[directory.size()] == '/');
}

std::string utf8_text(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace

void Debouncer::note(Clock::time_point now) noexcept {
    if (!pending_) {
        first_ = now;
        pending_ = true;
    }
    last_ = now;
}

bool Debouncer::due(Clock::time_point now) const noexcept {
    return pending_ && (now - last_ >= options_.quiet || now - first_ >= options_.max_wait);
}

std::optional<Clock::duration> Debouncer::until_due(Clock::time_point now) const noexcept {
    if (!pending_) {
        return std::nullopt;
    }
    const Clock::time_point deadline = std::min(last_ + options_.quiet, first_ + options_.max_wait);
    return now >= deadline ? Clock::duration::zero() : deadline - now;
}

core::Result<SourceSnapshot, std::string> take_snapshot(const core::Path& project, const std::optional<core::Path>& skip) {
    std::optional<std::string> skipped;
    if (skip) {
        skipped = relative_under(project.str(), skip->str());
    }

    const fs::path root = core::to_native_path(project);
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return core::failure("cannot scan " + project.str() + ": " + ec.message());
    }

    SourceSnapshot snapshot;
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            return core::failure("cannot scan " + project.str() + ": " + ec.message());
        }
        const fs::directory_entry& entry = *it;
        const std::string relative = utf8_text(entry.path().lexically_relative(root));
        if (skipped && is_within(*skipped, relative)) {
            it.disable_recursion_pending();
            continue;
        }
        // Links are not followed, so a link cannot pull in a file from outside the project.
        ec.clear();
        const bool is_file = entry.is_regular_file(ec) && !entry.is_symlink(ec);
        if (ec || !is_file) {
            ec.clear();
            continue;
        }
        auto path = core::Path::from_utf8(project.str() + "/" + relative);
        if (!path.ok()) {
            continue;
        }
        auto bytes = core::read_file(path.value());
        if (!bytes.ok()) {
            continue;  // a file that vanished or cannot be read is picked up by the next notification
        }
        snapshot.emplace(relative, assets::ContentHash::of(bytes.value()).to_hex());
    }
    return core::success(std::move(snapshot));
}

WatchSession::WatchSession(Options options, ImporterRegistry importers, ChangeSource& source, WatchOptions watch)
    : options_(std::move(options)), importers_(std::move(importers)), source_(source), debouncer_(watch) {
    if (options_.output) {
        output_relative_ = relative_under(options_.project.str(), options_.output->str());
    }
}

core::Result<Report, std::string> WatchSession::start() {
    auto snapshot = take_snapshot(options_.project, options_.output);
    if (!snapshot.ok()) {
        return core::failure(snapshot.error());
    }
    last_snapshot_ = std::move(snapshot).value();
    return convert();
}

core::Result<std::optional<Report>, std::string> WatchSession::step(std::chrono::milliseconds wait) {
    std::chrono::milliseconds timeout = wait;
    if (const auto until = debouncer_.until_due(Clock::now())) {
        timeout = std::min(wait, std::chrono::ceil<std::chrono::milliseconds>(*until));
    }
    const WatchEvents events = source_.poll(timeout);

    // A change inside the output directory is the converter's own write. It is dropped here, and the snapshot comparison
    // below would drop it too.
    const Clock::time_point arrived = Clock::now();
    if (events.overflow) {
        debouncer_.note(arrived);
    }
    for (const std::string& path : events.paths) {
        if (!inside_output(path)) {
            debouncer_.note(arrived);
            break;
        }
    }

    if (!debouncer_.due(Clock::now())) {
        return core::success(std::optional<Report>{});
    }
    debouncer_.clear();

    auto snapshot = take_snapshot(options_.project, options_.output);
    if (!snapshot.ok()) {
        return core::failure(snapshot.error());
    }
    // Nothing the converter reads has changed, so a pass would produce the same result. This is the loop guard.
    if (last_snapshot_ && *last_snapshot_ == snapshot.value()) {
        return core::success(std::optional<Report>{});
    }
    // The snapshot this pass is based on is the one stored. A change made while the pass runs differs from it, so the
    // next notification starts another pass.
    last_snapshot_ = std::move(snapshot).value();
    auto report = convert();
    if (!report.ok()) {
        return core::failure(report.error());
    }
    return core::success(std::optional<Report>(std::move(report).value()));
}

bool WatchSession::inside_output(const std::string& relative) const {
    return output_relative_ && is_within(*output_relative_, relative);
}

core::Result<Report, std::string> WatchSession::convert() {
    return run(Command::Cook, options_, importers_);
}

}  // namespace engine::converter
