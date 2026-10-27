#include "engine/core/assert.hpp"

#include "engine/core/log.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

namespace engine::core {
namespace {

void report_to_log(const AssertionFailure& failure) {
    std::string text = "Assertion failed: ";
    text += failure.expression;
    text += " (";
    text += failure.message;
    text += ") at ";
    text += failure.file;
    text += ':';
    text += std::to_string(failure.line);
    log_message(LogLevel::Fatal, "assert", text);
}

std::atomic<AssertHandler>& handler_slot() {
    static std::atomic<AssertHandler> slot{&report_to_log};
    return slot;
}

}  // namespace

AssertHandler set_assert_handler(AssertHandler handler) noexcept {
    return handler_slot().exchange(handler != nullptr ? handler : &report_to_log);
}

void assertion_failed(const AssertionFailure& failure) {
    handler_slot().load()(failure);
    std::abort();
}

}  // namespace engine::core
