#pragma once

// The editor console (US-061): a bounded log of every record the core logger emits plus the editor's own messages. The Console
// panel and the Errors panel read it. The oldest entries are dropped when the capacity is reached, and the drop count is kept.
// It is a LogSink, so the host adds it with core::add_log_sink.

#include "engine/core/log.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr std::size_t kDefaultConsoleCapacity = 2000;

struct ConsoleEntry {
    std::uint64_t sequence = 0;  // 1, 2, ... in the order the entries were added; never reused
    core::LogLevel level = core::LogLevel::Info;
    std::string category;  // valid UTF-8
    std::string message;   // valid UTF-8
};

// Replaces every byte of an invalid UTF-8 text with '?', so a log line from a foreign source never breaks the panel.
[[nodiscard]] std::string sanitize_utf8(std::string_view text);

class Console final : public core::LogSink {
public:
    explicit Console(std::size_t capacity = kDefaultConsoleCapacity);

    // Thread-safe: the logger calls it under its own lock, and the editor may add entries from the frame loop.
    void write(const core::LogRecord& record) override;
    void add(core::LogLevel level, std::string_view category, std::string_view message);

    // Every entry added from now on is also written to `sink`, whichever path added it (a log record or add()). The editor uses it for
    // --log, so a script that drives the window can read what the editor did. Pass null to stop.
    void set_mirror(std::shared_ptr<core::LogSink> sink);

    // The retained entries, oldest first.
    [[nodiscard]] std::vector<ConsoleEntry> snapshot() const;
    void clear();

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    // Entries dropped to keep the capacity since the console was created or last cleared.
    [[nodiscard]] std::uint64_t dropped() const;

private:
    void push_locked(core::LogLevel level, std::string_view category, std::string_view message);

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<ConsoleEntry> entries_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t dropped_ = 0;
    std::shared_ptr<core::LogSink> mirror_;
};

}  // namespace engine::editor
