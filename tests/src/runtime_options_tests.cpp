// Runtime command-line options (US-060): the packs, start scene, backend, and profile the standalone runtime accepts, and the
// usage errors it reports before it opens any file.

#include "engine/runtime/runtime_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using engine::core::Guid;
using engine::runtime::parse_runtime_options;

namespace {

constexpr const char* kStart = "3f2a1b0c-9d8e-4f7a-8b6c-5d4e3f2a1b01";

}  // namespace

TEST_CASE("Runtime options: the packs, start scene, backend, and profile are read as given", "[unit]") {
    auto parsed = parse_runtime_options({"--pack", "a.pack", "--start-scene", kStart, "--backend", "dx12", "--profile", "full",
                                         "--pack", "b.pack"});

    REQUIRE(parsed.ok());
    const auto& options = parsed.value();
    CHECK(options.packs == std::vector<std::string>{"a.pack", "b.pack"});
    CHECK(options.start_scene == Guid::parse(kStart).value());
    CHECK(options.backend == "dx12");
    CHECK(options.profile == "full");
    CHECK_FALSE(options.help);
}

TEST_CASE("Runtime options: the profile defaults to native and the backend to the platform's API", "[unit]") {
    auto parsed = parse_runtime_options({"--pack", "a.pack", "--start-scene", kStart});

    REQUIRE(parsed.ok());
#ifdef _WIN32
    CHECK(parsed.value().backend == "dx12");
#else
    CHECK(parsed.value().backend == "vulkan");
#endif
    CHECK(parsed.value().profile == "native");
}

TEST_CASE("Runtime options: a missing or invalid value is a usage error with a message", "[unit]") {
    auto no_pack = parse_runtime_options({"--start-scene", kStart});
    REQUIRE_FALSE(no_pack.ok());
    CHECK(no_pack.error() == "--pack is required");

    auto no_scene = parse_runtime_options({"--pack", "a.pack"});
    REQUIRE_FALSE(no_scene.ok());
    CHECK(no_scene.error() == "--start-scene is required");

    auto bad_guid = parse_runtime_options({"--pack", "a.pack", "--start-scene", "not-a-guid"});
    REQUIRE_FALSE(bad_guid.ok());
    CHECK(bad_guid.error().find("--start-scene requires a GUID") != std::string::npos);

    auto bad_backend = parse_runtime_options({"--pack", "a.pack", "--start-scene", kStart, "--backend", "metal"});
    REQUIRE_FALSE(bad_backend.ok());
    CHECK(bad_backend.error() == "invalid backend 'metal'");

    auto bad_profile = parse_runtime_options({"--pack", "a.pack", "--start-scene", kStart, "--profile", "fast"});
    REQUIRE_FALSE(bad_profile.ok());
    CHECK(bad_profile.error() == "invalid profile 'fast'");

    auto unknown = parse_runtime_options({"--bogus"});
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error() == "unknown argument: --bogus");

    auto no_value = parse_runtime_options({"--pack"});
    REQUIRE_FALSE(no_value.ok());
    CHECK(no_value.error() == "--pack requires a value");
}

TEST_CASE("Runtime options: help is accepted without the required arguments", "[unit]") {
    auto parsed = parse_runtime_options({"--help"});

    REQUIRE(parsed.ok());
    CHECK(parsed.value().help);
    CHECK(engine::runtime::runtime_usage_text().find("--start-scene") != std::string::npos);
}
