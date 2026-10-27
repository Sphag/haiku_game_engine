#include "engine/core/status.hpp"
#include "engine/runtime/lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>

using engine::core::StatusCode;
using engine::core::to_string;
using engine::runtime::Lifecycle;
using engine::runtime::LifecycleState;

TEST_CASE("A new lifecycle is Created", "[unit]") {
    const Lifecycle lifecycle;

    CHECK(lifecycle.state() == LifecycleState::Created);
}

TEST_CASE("Lifecycle start moves Created to Running", "[unit]") {
    Lifecycle lifecycle;

    const auto status = lifecycle.start();

    CHECK(status.ok());
    CHECK(lifecycle.state() == LifecycleState::Running);
}

TEST_CASE("Lifecycle stop moves Running to Stopped", "[unit]") {
    Lifecycle lifecycle;
    REQUIRE(lifecycle.start().ok());

    const auto status = lifecycle.stop();

    CHECK(status.ok());
    CHECK(lifecycle.state() == LifecycleState::Stopped);
}

TEST_CASE("Lifecycle rejects stop before start without changing state", "[unit]") {
    Lifecycle lifecycle;

    const auto status = lifecycle.stop();

    CHECK(status.code == StatusCode::InvalidState);
    CHECK(lifecycle.state() == LifecycleState::Created);
}

TEST_CASE("Lifecycle rejects a second start", "[unit]") {
    Lifecycle lifecycle;
    REQUIRE(lifecycle.start().ok());

    const auto status = lifecycle.start();

    CHECK(status.code == StatusCode::InvalidState);
    CHECK(lifecycle.state() == LifecycleState::Running);
}

TEST_CASE("Lifecycle is single-use after stop", "[unit]") {
    Lifecycle lifecycle;
    REQUIRE(lifecycle.start().ok());
    REQUIRE(lifecycle.stop().ok());

    CHECK(lifecycle.start().code == StatusCode::InvalidState);
    CHECK(lifecycle.stop().code == StatusCode::InvalidState);
    CHECK(lifecycle.state() == LifecycleState::Stopped);
}

TEST_CASE("Lifecycle state names are stable", "[unit]") {
    CHECK(to_string(LifecycleState::Created) == "Created");
    CHECK(to_string(LifecycleState::Running) == "Running");
    CHECK(to_string(LifecycleState::Stopped) == "Stopped");
}

TEST_CASE("Full lifecycle reports core status names at each step", "[integration]") {
    Lifecycle lifecycle;

    CHECK(to_string(lifecycle.stop().code) == "InvalidState");
    CHECK(to_string(lifecycle.start().code) == "Ok");
    CHECK(to_string(lifecycle.state()) == "Running");
    CHECK(to_string(lifecycle.stop().code) == "Ok");
    CHECK(to_string(lifecycle.state()) == "Stopped");
}
