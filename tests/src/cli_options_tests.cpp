#include "cli_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

using engine::tests::CliParseResult;
using engine::tests::Suite;
using engine::tests::parse_command_line;

namespace {

CliParseResult parse(std::vector<std::string_view> args) {
    return parse_command_line(args);
}

}  // namespace

TEST_CASE("CLI defaults to running every suite", "[unit]") {
    const auto result = parse({});

    REQUIRE(result.error.empty());
    REQUIRE_FALSE(result.options.show_help);
    REQUIRE(result.options.suite == Suite::All);
}

TEST_CASE("CLI recognizes long and short help flags", "[unit]") {
    CHECK(parse({"--help"}).options.show_help);
    CHECK(parse({"-h"}).options.show_help);
    CHECK(parse({"-h"}).error.empty());
}

TEST_CASE("CLI accepts a suite as a separate value or with equals", "[unit]") {
    const auto separate = parse({"--suite", "unit"});
    REQUIRE(separate.error.empty());
    CHECK(separate.options.suite == Suite::Unit);

    const auto joined = parse({"--suite=integration"});
    REQUIRE(joined.error.empty());
    CHECK(joined.options.suite == Suite::Integration);

    CHECK(parse({"--suite", "all"}).options.suite == Suite::All);
}

TEST_CASE("CLI rejects unknown arguments with the offending text", "[unit]") {
    const auto result = parse({"--bogus"});

    CHECK(result.error == "unknown argument: --bogus");
}

TEST_CASE("CLI rejects a suite flag without a value", "[unit]") {
    const auto result = parse({"--suite"});

    CHECK(result.error == "--suite requires a value");
}

TEST_CASE("CLI rejects suites outside the supported set", "[unit]") {
    const auto result = parse({"--suite", "bogus"});

    CHECK(result.error == "invalid suite 'bogus' (expected unit, integration, gpu, editor, or all)");
}

TEST_CASE("CLI selects the gpu suite only with a backend", "[unit]") {
    const auto missing = parse({"--suite", "gpu"});
    CHECK(missing.error == "--suite gpu requires --backend <dx12|vulkan>");

    const auto joined = parse({"--suite=gpu", "--backend=dx12"});
    REQUIRE(joined.error.empty());
    CHECK(joined.options.suite == Suite::Gpu);
    CHECK(joined.options.backend == "dx12");

    const auto separate = parse({"--backend", "vulkan", "--suite", "gpu"});
    REQUIRE(separate.error.empty());
    CHECK(separate.options.backend == "vulkan");
}

TEST_CASE("CLI rejects unknown backends and a backend without the gpu suite", "[unit]") {
    CHECK(parse({"--suite", "gpu", "--backend", "metal"}).error == "invalid backend 'metal' (expected dx12 or vulkan)");
    CHECK(parse({"--backend"}).error == "--backend requires a value");
    CHECK(parse({"--backend", "dx12"}).error == "--backend applies only to --suite gpu or --suite editor");
}

TEST_CASE("CLI stops at the first error even when help came earlier", "[unit]") {
    const auto result = parse({"--help", "--bogus"});

    CHECK(result.error == "unknown argument: --bogus");
}

TEST_CASE("Each suite maps to its Catch2 tag expression", "[unit]") {
    // The CPU suites never include GPU tests, so a default run cannot open a GPU.
    CHECK(engine::tests::suite_tag_expression(Suite::All, "") == "~[gpu]");
    CHECK(engine::tests::suite_tag_expression(Suite::Unit, "") == "[unit]");
    CHECK(engine::tests::suite_tag_expression(Suite::Integration, "") == "[integration]");
    CHECK(engine::tests::suite_tag_expression(Suite::Gpu, "dx12") == "[gpu][dx12]");
}

TEST_CASE("CLI selects the editor suite, with an optional backend", "[unit]") {
    const auto cpu = parse({"--suite", "editor"});
    REQUIRE(cpu.error.empty());
    CHECK(cpu.options.suite == Suite::Editor);
    CHECK(engine::tests::suite_tag_expression(Suite::Editor, "") == "[editor]~[gpu]");

    const auto gpu = parse({"--suite=editor", "--backend=vulkan"});
    REQUIRE(gpu.error.empty());
    CHECK(gpu.options.backend == "vulkan");
    CHECK(engine::tests::suite_tag_expression(Suite::Editor, "vulkan") == "[editor]~[gpu],[editor][gpu][vulkan]");
}

TEST_CASE("Usage text documents suites and exit codes", "[unit]") {
    const auto usage = engine::tests::usage_text();

    CHECK(usage.find("--suite <unit|integration|gpu|editor|all>") != std::string::npos);
    CHECK(usage.find("--backend <dx12|vulkan>") != std::string::npos);
    CHECK(usage.find("3    the requested GPU backend") != std::string::npos);
    CHECK(usage.find("2    invalid command line") != std::string::npos);
}
