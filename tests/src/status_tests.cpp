#include "engine/core/status.hpp"

#include <catch2/catch_test_macros.hpp>

using engine::core::Status;
using engine::core::StatusCode;
using engine::core::to_string;

TEST_CASE("Default status is Ok", "[unit]") {
    const Status status;

    CHECK(status.ok());
    CHECK(status.code == StatusCode::Ok);
}

TEST_CASE("Non-Ok status reports not ok", "[unit]") {
    const Status status{StatusCode::InvalidState};

    CHECK_FALSE(status.ok());
}

TEST_CASE("Status codes have stable names", "[unit]") {
    CHECK(to_string(StatusCode::Ok) == "Ok");
    CHECK(to_string(StatusCode::InvalidState) == "InvalidState");
}
