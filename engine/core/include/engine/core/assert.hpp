#pragma once

#include <string_view>

namespace engine::core {

struct AssertionFailure {
    std::string_view expression;
    std::string_view message;
    std::string_view file;
    int line;
};

// Runs before the process stops. A handler may throw (tests do) but must not return normally.
using AssertHandler = void (*)(const AssertionFailure&);

// Installs a handler and returns the previous one. Passing nullptr restores the default handler.
AssertHandler set_assert_handler(AssertHandler handler) noexcept;

// Reports the failure through the handler; if the handler returns, the process aborts.
[[noreturn]] void assertion_failed(const AssertionFailure& failure);

}  // namespace engine::core

// ENGINE_ASSERT is active in every configuration. ENGINE_DEBUG_ASSERT is compiled out when NDEBUG is defined.
#define ENGINE_ASSERT(expression, message)                                                          \
    do {                                                                                            \
        if (!(expression)) {                                                                        \
            ::engine::core::assertion_failed({#expression, (message), __FILE__, __LINE__});         \
        }                                                                                           \
    } while (false)

#ifdef NDEBUG
#define ENGINE_DEBUG_ASSERT(expression, message) \
    do {                                         \
    } while (false)
#else
#define ENGINE_DEBUG_ASSERT(expression, message) ENGINE_ASSERT(expression, message)
#endif
