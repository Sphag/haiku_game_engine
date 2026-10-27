#pragma once

#include "engine/core/assert.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace engine::core {

// Wrappers that select the Result alternative at the call site: `return success(x);` or `return failure(e);`.
template <typename T>
struct Success {
    T value;
};

template <typename E>
struct Failure {
    E error;
};

// Payload for operations that produce no value on success.
struct Unit {};

template <typename T>
[[nodiscard]] Success<std::decay_t<T>> success(T&& value) {
    return Success<std::decay_t<T>>{std::forward<T>(value)};
}

[[nodiscard]] inline Success<Unit> success() {
    return Success<Unit>{};
}

template <typename E>
[[nodiscard]] Failure<std::decay_t<E>> failure(E&& error) {
    return Failure<std::decay_t<E>>{std::forward<E>(error)};
}

// Holds either a success value of type T or an error of type E. Check ok() before reading either.
template <typename T, typename E>
class [[nodiscard]] Result {
public:
    Result(Success<T> ok_value) : storage_(std::in_place_index<0>, std::move(ok_value.value)) {}
    Result(Failure<E> failure_value) : storage_(std::in_place_index<1>, std::move(failure_value.error)) {}

    [[nodiscard]] bool ok() const noexcept {
        return storage_.index() == 0;
    }
    explicit operator bool() const noexcept {
        return ok();
    }

    [[nodiscard]] T& value() & {
        ENGINE_ASSERT(ok(), "Result::value() called on an error");
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        ENGINE_ASSERT(ok(), "Result::value() called on an error");
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& value() && {
        ENGINE_ASSERT(ok(), "Result::value() called on an error");
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] E& error() & {
        ENGINE_ASSERT(!ok(), "Result::error() called on a success");
        return std::get<1>(storage_);
    }
    [[nodiscard]] const E& error() const& {
        ENGINE_ASSERT(!ok(), "Result::error() called on a success");
        return std::get<1>(storage_);
    }

private:
    std::variant<T, E> storage_;
};

}  // namespace engine::core
