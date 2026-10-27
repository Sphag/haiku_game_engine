#include "engine/core/log.hpp"

#include "engine/core/clock.hpp"

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace engine::core {
namespace {

struct LoggerState {
    std::mutex mutex;
    std::vector<std::shared_ptr<LogSink>> sinks{std::make_shared<StderrLogSink>()};
    LogLevel min_level = LogLevel::Info;
};

LoggerState& state() {
    static LoggerState instance;
    return instance;
}

}  // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "Trace";
        case LogLevel::Debug:
            return "Debug";
        case LogLevel::Info:
            return "Info";
        case LogLevel::Warning:
            return "Warning";
        case LogLevel::Error:
            return "Error";
        case LogLevel::Fatal:
            return "Fatal";
    }
    return "Unknown";
}

StderrLogSink::StderrLogSink() {
#ifdef _WIN32
    // Without this, the Windows console shows UTF-8 text (such as Cyrillic) as mojibake.
    SetConsoleOutputCP(CP_UTF8);
#endif
}

void StderrLogSink::write(const LogRecord& record) {
    std::string line = "[";
    line += to_string(record.level);
    line += "] ";
    line += record.category;
    line += ": ";
    line += record.message;
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

void MemoryLogSink::write(const LogRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(Entry{record.level, std::string(record.category), std::string(record.message)});
}

std::vector<MemoryLogSink::Entry> MemoryLogSink::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

void add_log_sink(std::shared_ptr<LogSink> sink) {
    auto& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    logger.sinks.push_back(std::move(sink));
}

void remove_log_sink(const std::shared_ptr<LogSink>& sink) {
    auto& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    logger.sinks.erase(std::remove(logger.sinks.begin(), logger.sinks.end(), sink), logger.sinks.end());
}

void set_min_log_level(LogLevel level) noexcept {
    auto& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    logger.min_level = level;
}

LogLevel min_log_level() noexcept {
    auto& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    return logger.min_level;
}

void log_message(LogLevel level, std::string_view category, std::string_view message) {
    auto& logger = state();
    std::lock_guard<std::mutex> lock(logger.mutex);
    if (level < logger.min_level) {
        return;
    }
    const LogRecord record{level, category, message, MonotonicClock::now()};
    for (const auto& sink : logger.sinks) {
        sink->write(record);
    }
}

}  // namespace engine::core
