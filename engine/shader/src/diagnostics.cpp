#include "engine/shader/diagnostics.hpp"

#include <cstddef>
#include <cstdio>
#include <utility>

namespace engine::shader {
namespace {

struct Marker {
    std::string_view text;
    Severity severity;
};

constexpr Marker kMarkers[] = {
    {": error: ", Severity::Error},
    {": warning: ", Severity::Warning},
    {": note: ", Severity::Note},
};

bool all_digits(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

std::uint32_t to_number(std::string_view digits) noexcept {
    std::uint32_t value = 0;
    for (const char c : digits) {
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

// Parses one output line. Returns false for a blank line and for an indented source excerpt.
bool parse_line(std::string_view line, Diagnostic& out) {
    // The compiler prints the source line and a caret under an error, indented. They are context, not diagnostics.
    if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        return false;
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    line.remove_prefix(start);
    if (line.empty()) {
        return false;
    }

    // The first marker that appears is the severity separator. Its text before the marker holds the location.
    std::size_t best = std::string_view::npos;
    const Marker* chosen = nullptr;
    for (const Marker& marker : kMarkers) {
        const std::size_t at = line.find(marker.text);
        if (at != std::string_view::npos && (best == std::string_view::npos || at < best)) {
            best = at;
            chosen = &marker;
        }
    }

    if (chosen != nullptr) {
        const std::string_view prefix = line.substr(0, best);
        const std::string_view message = line.substr(best + chosen->text.size());
        const std::size_t col_colon = prefix.rfind(':');
        const std::size_t line_colon = col_colon == std::string_view::npos || col_colon == 0 ? std::string_view::npos
                                                                                           : prefix.rfind(':', col_colon - 1);
        if (line_colon != std::string_view::npos) {
            const std::string_view col_text = prefix.substr(col_colon + 1);
            const std::string_view line_text = prefix.substr(line_colon + 1, col_colon - line_colon - 1);
            if (all_digits(col_text) && all_digits(line_text)) {
                out.severity = chosen->severity;
                out.file = std::string(prefix.substr(0, line_colon));
                out.line = to_number(line_text);
                out.column = to_number(col_text);
                out.message = std::string(message);
                return true;
            }
        }
        // A marker without a location, such as "error: ..." after a tool name: keep the whole line.
    }

    out.severity = Severity::Error;
    out.file.clear();
    out.line = 0;
    out.column = 0;
    out.message = std::string(line);
    return true;
}

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out += c;
                }
                break;
        }
    }
    out += '"';
}

}  // namespace

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::Error:
            return "error";
        case Severity::Warning:
            return "warning";
        case Severity::Note:
            return "note";
    }
    return "error";
}

std::vector<Diagnostic> parse_compiler_output(std::string_view output) {
    std::vector<Diagnostic> diagnostics;
    std::size_t begin = 0;
    while (begin <= output.size()) {
        std::size_t end = output.find('\n', begin);
        if (end == std::string_view::npos) {
            end = output.size();
        }
        Diagnostic diagnostic;
        if (parse_line(output.substr(begin, end - begin), diagnostic)) {
            diagnostics.push_back(std::move(diagnostic));
        }
        begin = end + 1;
    }
    return diagnostics;
}

std::string diagnostics_to_text(const std::vector<Diagnostic>& diagnostics) {
    std::string out;
    for (const Diagnostic& diagnostic : diagnostics) {
        if (!diagnostic.file.empty() || diagnostic.line != 0) {
            out += diagnostic.file;
            out += ':';
            out += std::to_string(diagnostic.line);
            out += ':';
            out += std::to_string(diagnostic.column);
            out += ": ";
        }
        out += to_string(diagnostic.severity);
        out += ": ";
        out += diagnostic.message;
        out += '\n';
    }
    return out;
}

std::string diagnostics_to_json(std::string_view source, const std::vector<Diagnostic>& diagnostics) {
    std::string out = "{\"schema\": \"engine.shader_diagnostics\", \"version\": 1, \"source\": ";
    append_json_string(out, source);
    out += ", \"status\": \"";
    out += has_errors(diagnostics) ? "error" : "ok";
    out += "\", \"diagnostics\": [";
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        const Diagnostic& diagnostic = diagnostics[i];
        out += i == 0 ? "\n  " : ",\n  ";
        out += "{\"severity\": \"";
        out += to_string(diagnostic.severity);
        out += "\", \"file\": ";
        append_json_string(out, diagnostic.file);
        out += ", \"line\": " + std::to_string(diagnostic.line);
        out += ", \"column\": " + std::to_string(diagnostic.column);
        out += ", \"message\": ";
        append_json_string(out, diagnostic.message);
        out += '}';
    }
    out += diagnostics.empty() ? "]}" : "\n]}";
    return out;
}

bool has_errors(const std::vector<Diagnostic>& diagnostics) noexcept {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Error) {
            return true;
        }
    }
    return false;
}

}  // namespace engine::shader
