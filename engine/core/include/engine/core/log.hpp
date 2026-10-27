#pragma once

#include "engine/core/units.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

struct LogRecord {
    LogLevel level;
    std::string_view category;  // valid only for the duration of write()
    std::string_view message;   // valid only for the duration of write()
    units::Seconds timestamp;   // MonotonicClock reading taken when the message was logged
};

// Sinks are called under the logger's lock, so they must not log.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

// Writes "[Level] category: message" lines to stderr as UTF-8.
class StderrLogSink final : public LogSink {
public:
    StderrLogSink();
    void write(const LogRecord& record) override;
};

// Keeps copies of every record it receives; used by tests and diagnostics.
class MemoryLogSink final : public LogSink {
public:
    struct Entry {
        LogLevel level;
        std::string category;
        std::string message;
    };

    void write(const LogRecord& record) override;
    [[nodiscard]] std::vector<Entry> entries() const;

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

// The logger starts with one StderrLogSink and a minimum level of Info.
void add_log_sink(std::shared_ptr<LogSink> sink);
void remove_log_sink(const std::shared_ptr<LogSink>& sink);
void set_min_log_level(LogLevel level) noexcept;
[[nodiscard]] LogLevel min_log_level() noexcept;

// Records below the minimum level are dropped before reaching any sink.
void log_message(LogLevel level, std::string_view category, std::string_view message);

}  // namespace engine::core
