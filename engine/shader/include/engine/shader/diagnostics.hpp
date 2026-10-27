#pragma once

// Compiler diagnostics (US-031) in two formats: text for people and JSON for tools. Both come from the same list, so a
// build log and a machine report never disagree.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::shader {

enum class Severity : std::uint8_t {
    Error,
    Warning,
    Note,
};
[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string file;         // source path as the compiler reported it; empty when the message has no location
    std::uint32_t line = 0;   // 1-based; 0 when the message has no location
    std::uint32_t column = 0;  // 1-based; 0 when unknown
    std::string message;
};

// Parses compiler output. A located line has the form "<file>:<line>:<column>: <severity>: <message>", and the file
// may itself contain a drive letter. Any other non-empty line becomes an error without a location.
[[nodiscard]] std::vector<Diagnostic> parse_compiler_output(std::string_view output);

// One line per diagnostic: "<file>:<line>:<column>: <severity>: <message>", or "<severity>: <message>" without a location.
[[nodiscard]] std::string diagnostics_to_text(const std::vector<Diagnostic>& diagnostics);

// {"schema": "engine.shader_diagnostics", "version": 1, "source": ..., "status": "ok" | "error", "diagnostics": [...]}.
// status is "error" when any diagnostic has Error severity. Strings are escaped as JSON.
[[nodiscard]] std::string diagnostics_to_json(std::string_view source, const std::vector<Diagnostic>& diagnostics);

[[nodiscard]] bool has_errors(const std::vector<Diagnostic>& diagnostics) noexcept;

}  // namespace engine::shader
