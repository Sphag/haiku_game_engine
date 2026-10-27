#pragma once

// Shared fixtures for the material graph tests (US-067): a graph that uses every node kind, and the text edits the tests make to it.

#include "engine/material_graph/graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace engine::material_graph::tests {

// Textures: albedo (sRGB, white when unbound) and normal (linear, a flat normal when unbound). Parameters: tint (vec4), roughness, normal_scale. The output is a PBR output with
// base color, roughness, and normal connected; the other pins keep their defaults.
inline std::string sample_graph_json() {
    return R"({
  "schema": "engine.material_graph",
  "version": 1,
  "name": "sample",
  "textures": [
    {"name": "albedo", "color_space": "srgb"},
    {"name": "normal", "color_space": "linear", "fallback": "flat_normal"}
  ],
  "nodes": [
    {"id": "uv", "kind": "uv"},
    {"id": "albedo_tex", "kind": "texture_sample", "name": "albedo", "inputs": {"uv": "uv"}},
    {"id": "tint", "kind": "parameter", "type": "vec4", "name": "tint", "value": [1, 1, 1, 1]},
    {"id": "tinted", "kind": "arithmetic", "op": "multiply", "inputs": {"a": "albedo_tex", "b": "tint"}},
    {"id": "rough", "kind": "parameter", "type": "float", "name": "roughness", "value": [0.25]},
    {"id": "normal_tex", "kind": "texture_sample", "name": "normal", "inputs": {"uv": "uv"}},
    {"id": "normal_scale", "kind": "parameter", "type": "float", "name": "normal_scale", "value": [1.0]},
    {"id": "normal", "kind": "normal_map", "inputs": {"texture": "normal_tex", "scale": "normal_scale"}},
    {"id": "out", "kind": "pbr_output", "inputs": {"base_color": "tinted", "roughness": "rough", "normal": "normal"}}
  ]
})";
}

// Replaces the first occurrence of `from` with `to`. The fixture text must contain it, or the test is wrong.
inline std::string edit(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// The graph of a document that must be valid.
inline Graph parse_valid(const std::string& text) {
    auto parsed = parse_graph(text);
    if (!parsed.ok()) {
        std::string message;
        for (const GraphError& error : parsed.error()) {
            message += to_text(error) + "\n";
        }
        FAIL(message);
    }
    return std::move(parsed).value();
}

// The errors of a document, as text. Empty when it parses.
inline std::string errors_of(const std::string& text) {
    auto parsed = parse_graph(text);
    if (parsed.ok()) {
        return {};
    }
    std::string message;
    for (const GraphError& error : parsed.error()) {
        message += to_text(error) + "\n";
    }
    return message;
}

}  // namespace engine::material_graph::tests
