#include "engine/core/assert.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

namespace {

std::string g_expression;
std::string g_message;
int g_line = 0;

void throwing_handler(const engine::core::AssertionFailure& failure) {
    g_expression = std::string(failure.expression);
    g_message = std::string(failure.message);
    g_line = failure.line;
    throw std::runtime_error("assertion");
}

// Installs the throwing handler for one test and restores the previous handler afterwards.
class ScopedAssertHandler {
public:
    ScopedAssertHandler() : previous_(engine::core::set_assert_handler(&throwing_handler)) {}
    ~ScopedAssertHandler() {
        engine::core::set_assert_handler(previous_);
    }

private:
    engine::core::AssertHandler previous_;
};

}  // namespace

// Wrapped in functions because ENGINE_ASSERT is a statement and cannot be a CHECK_* argument.
void passing_arithmetic_assert() {
    // volatile keeps the condition from being a compile-time constant (MSVC C4127 is an error under /WX).
    volatile int one = 1;
    ENGINE_ASSERT(one + 1 == 2, "arithmetic");
}

TEST_CASE("A true assertion does nothing", "[unit]") {
    const ScopedAssertHandler guard;

    CHECK_NOTHROW(passing_arithmetic_assert());
}

// Wrapped in a function so the macro's commas do not split CHECK_THROWS_AS arguments.
int g_failing_line = 0;

void failing_ordering_assert() {
    volatile int big = 2;
    g_failing_line = __LINE__ + 1;
    ENGINE_ASSERT(big < 1, "ordering");
}

#ifndef NDEBUG
void failing_debug_assert() {
    volatile bool never = false;
    ENGINE_DEBUG_ASSERT(never, "active in debug");
}
#endif

TEST_CASE("A failed assertion passes the expression, message, and line to the handler", "[unit]") {
    const ScopedAssertHandler guard;

    CHECK_THROWS_AS(failing_ordering_assert(), std::runtime_error);
    CHECK(g_expression == "big < 1");
    CHECK(g_message == "ordering");
    CHECK(g_line == g_failing_line);
}

TEST_CASE("set_assert_handler returns the previous handler", "[unit]") {
    const auto installed = engine::core::set_assert_handler(&throwing_handler);
    const auto previous = engine::core::set_assert_handler(installed);

    CHECK(previous == &throwing_handler);
}

TEST_CASE("Debug assertions follow the build configuration", "[unit]") {
    const ScopedAssertHandler guard;

#ifdef NDEBUG
    // The macro expands to a statement, so it runs outside any CHECK; reaching the line proves no handler ran.
    ENGINE_DEBUG_ASSERT(false, "compiled out in release");
    const bool reached = true;
    CHECK(reached);
#else
    CHECK_THROWS_AS(failing_debug_assert(), std::runtime_error);
#endif
}
