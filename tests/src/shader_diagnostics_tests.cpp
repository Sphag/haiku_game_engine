// Compiler diagnostics (US-031): the text and JSON reports come from one list, so they must agree. These are pure string tests.

#include "engine/shader/diagnostics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using engine::shader::Diagnostic;
using engine::shader::diagnostics_to_json;
using engine::shader::diagnostics_to_text;
using engine::shader::has_errors;
using engine::shader::parse_compiler_output;
using engine::shader::Severity;

TEST_CASE("A located compiler error keeps its file, line, column, and message", "[unit]") {
    const auto diagnostics = parse_compiler_output("shaders/fixture_error.hlsl:7:5: error: unknown identifier 'Bad'\n");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].severity == Severity::Error);
    CHECK(diagnostics[0].file == "shaders/fixture_error.hlsl");
    CHECK(diagnostics[0].line == 7);
    CHECK(diagnostics[0].column == 5);
    CHECK(diagnostics[0].message == "unknown identifier 'Bad'");
}

TEST_CASE("A Windows drive letter in the file name is not mistaken for the line", "[unit]") {
    const auto diagnostics = parse_compiler_output("D:/dev/shaders/main.hlsl:12:3: warning: unused value\r\n");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].severity == Severity::Warning);
    CHECK(diagnostics[0].file == "D:/dev/shaders/main.hlsl");
    CHECK(diagnostics[0].line == 12);
    CHECK(diagnostics[0].column == 3);
}

TEST_CASE("Lines without a location are kept as errors, and blank lines are dropped", "[unit]") {
    const auto diagnostics = parse_compiler_output("\nfatal error: no output file\n\nerror: failed to open include\n");
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].severity == Severity::Error);
    CHECK(diagnostics[0].file.empty());
    CHECK(diagnostics[0].line == 0);
    CHECK(diagnostics[0].message == "fatal error: no output file");
    CHECK(diagnostics[1].message == "error: failed to open include");
    CHECK(has_errors(diagnostics));
}

TEST_CASE("The source excerpt and caret that the compiler prints under an error are not diagnostics", "[unit]") {
    const auto diagnostics = parse_compiler_output(
        "shaders/fixture_error.hlsl:7:20: error: use of undeclared identifier 'undefined_symbol'\n"
        "    Output[id.x] = undefined_symbol;\n"
        "                   ^\n");
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].line == 7);
}

TEST_CASE("The text report prints one compiler-style line per diagnostic", "[unit]") {
    const std::vector<Diagnostic> diagnostics = {
        Diagnostic{Severity::Error, "a.hlsl", 7, 5, "bad token"},
        Diagnostic{Severity::Warning, "a.hlsl", 9, 1, "unused"},
        Diagnostic{Severity::Note, std::string(), 0, 0, "no location"},
    };
    CHECK(diagnostics_to_text(diagnostics) == "a.hlsl:7:5: error: bad token\n"
                                              "a.hlsl:9:1: warning: unused\n"
                                              "note: no location\n");
}

TEST_CASE("The JSON report has a status, escapes its strings, and lists every diagnostic", "[unit]") {
    const std::vector<Diagnostic> diagnostics = {
        Diagnostic{Severity::Error, "a \"quoted\" path.hlsl", 3, 2, "line one\nline\ttwo \\ end"},
    };
    const std::string json = diagnostics_to_json("src/a.hlsl", diagnostics);
    CHECK(json.find("\"schema\": \"engine.shader_diagnostics\"") != std::string::npos);
    CHECK(json.find("\"source\": \"src/a.hlsl\"") != std::string::npos);
    CHECK(json.find("\"status\": \"error\"") != std::string::npos);
    CHECK(json.find("\"file\": \"a \\\"quoted\\\" path.hlsl\"") != std::string::npos);
    CHECK(json.find("\"message\": \"line one\\nline\\ttwo \\\\ end\"") != std::string::npos);
    CHECK(json.find("\"line\": 3") != std::string::npos);
}

TEST_CASE("A report with only warnings is ok, and an empty report is ok", "[unit]") {
    const std::vector<Diagnostic> warnings = {Diagnostic{Severity::Warning, "a.hlsl", 1, 1, "style"}};
    CHECK_FALSE(has_errors(warnings));
    CHECK(diagnostics_to_json("a.hlsl", warnings).find("\"status\": \"ok\"") != std::string::npos);
    CHECK(diagnostics_to_json("a.hlsl", {}).find("\"diagnostics\": []") != std::string::npos);
}
