#include "engine/core/result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::core::failure;
using engine::core::Result;
using engine::core::success;
using engine::core::Unit;

namespace {

Result<std::string, int> parse_number(bool valid) {
    if (!valid) {
        return failure(7);
    }
    return success(std::string("value"));
}

}  // namespace

TEST_CASE("Result holds a success value", "[unit]") {
    const auto result = parse_number(true);

    CHECK(result.ok());
    CHECK(static_cast<bool>(result));
    CHECK(result.value() == "value");
}

TEST_CASE("Result holds an error", "[unit]") {
    const auto result = parse_number(false);

    CHECK_FALSE(result.ok());
    CHECK(result.error() == 7);
}

TEST_CASE("Result moves its value out of an rvalue", "[unit]") {
    std::string moved = std::move(parse_number(true)).value();

    CHECK(moved == "value");
}

TEST_CASE("Result without a payload uses Unit", "[unit]") {
    const Result<Unit, int> done = success();

    CHECK(done.ok());
}
