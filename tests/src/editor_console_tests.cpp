// The editor console (US-061): the bounded log, the UTF-8 sanitizing of foreign text, and the core logger's sink path.

#include "engine/core/log.hpp"
#include "engine/editor/console.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using engine::core::LogLevel;
using engine::editor::Console;
using engine::editor::sanitize_utf8;

TEST_CASE("The console keeps the newest entries and counts what it dropped", "[unit][editor]") {
    Console console(3);
    for (int i = 1; i <= 5; ++i) {
        console.add(LogLevel::Info, "test", "entry " + std::to_string(i));
    }
    const auto entries = console.snapshot();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].message == "entry 3");
    CHECK(entries[2].message == "entry 5");
    CHECK(entries[0].sequence == 3);
    CHECK(entries[2].sequence == 5);
    CHECK(console.dropped() == 2);

    console.clear();
    CHECK(console.snapshot().empty());
    CHECK(console.dropped() == 0);
}

TEST_CASE("Foreign text is sanitized byte by byte, and valid Cyrillic is kept", "[unit][editor]") {
    CHECK(sanitize_utf8("Проект") == "Проект");
    CHECK(sanitize_utf8("ok\xFF") == "ok?");
    CHECK(sanitize_utf8("Пр\xD0") == "Пр?");
    CHECK(sanitize_utf8("\xE2\x82") == "??");
    CHECK(sanitize_utf8("\x80Ёж") == "?Ёж");
}

TEST_CASE("The core logger writes into an installed console and not after it is removed", "[unit][editor]") {
    auto console = std::make_shared<Console>();
    engine::core::add_log_sink(console);
    engine::core::log_message(LogLevel::Warning, "console-test", "сигнал через логгер");
    engine::core::remove_log_sink(console);
    engine::core::log_message(LogLevel::Warning, "console-test", "after removal");

    const auto entries = console->snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].level == LogLevel::Warning);
    CHECK(entries[0].category == "console-test");
    CHECK(entries[0].message == "сигнал через логгер");
}
