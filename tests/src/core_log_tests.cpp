#include "engine/core/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using engine::core::LogLevel;
using engine::core::log_message;
using engine::core::MemoryLogSink;

namespace {

// Captures every record at Trace and restores the previous minimum level when the test ends.
class CapturedLog {
public:
    CapturedLog() : previous_level_(engine::core::min_log_level()), sink_(std::make_shared<MemoryLogSink>()) {
        engine::core::set_min_log_level(LogLevel::Trace);
        engine::core::add_log_sink(sink_);
    }
    ~CapturedLog() {
        engine::core::remove_log_sink(sink_);
        engine::core::set_min_log_level(previous_level_);
    }

    [[nodiscard]] const MemoryLogSink& sink() const {
        return *sink_;
    }

private:
    LogLevel previous_level_;
    std::shared_ptr<MemoryLogSink> sink_;
};

}  // namespace

TEST_CASE("Log records reach a sink with level, category, and message", "[unit]") {
    const CapturedLog captured;

    log_message(LogLevel::Warning, "io", "disk is slow");

    const auto entries = captured.sink().entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].level == LogLevel::Warning);
    CHECK(entries[0].category == "io");
    CHECK(entries[0].message == "disk is slow");
}

TEST_CASE("Records below the minimum level are dropped", "[unit]") {
    const CapturedLog captured;
    engine::core::set_min_log_level(LogLevel::Error);

    log_message(LogLevel::Info, "core", "ignored");
    log_message(LogLevel::Error, "core", "kept");

    const auto entries = captured.sink().entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].message == "kept");
}

TEST_CASE("Cyrillic messages and categories are preserved as UTF-8", "[unit]") {
    const CapturedLog captured;

    log_message(LogLevel::Info, "файлы", "Привет, мир!");

    const auto entries = captured.sink().entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].category == "файлы");
    CHECK(entries[0].message == "Привет, мир!");
}

TEST_CASE("A removed sink receives no further records", "[unit]") {
    auto sink = std::make_shared<MemoryLogSink>();
    const auto previous_level = engine::core::min_log_level();
    engine::core::set_min_log_level(LogLevel::Trace);
    engine::core::add_log_sink(sink);
    log_message(LogLevel::Info, "core", "before");

    engine::core::remove_log_sink(sink);
    log_message(LogLevel::Info, "core", "after");
    engine::core::set_min_log_level(previous_level);

    const auto entries = sink->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].message == "before");
}

TEST_CASE("Level names are stable", "[unit]") {
    CHECK(engine::core::to_string(LogLevel::Trace) == "Trace");
    CHECK(engine::core::to_string(LogLevel::Fatal) == "Fatal");
}
