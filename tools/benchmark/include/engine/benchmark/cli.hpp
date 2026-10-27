#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bench {

struct Options {
    bool show_help = false;
    std::string suite;               // smoke or acceptance
    std::string backend;             // dx12 or vulkan; required for acceptance
    std::string output;              // report folder; required to run
    std::string manifest;            // defaults to the repository manifest
    std::optional<double> warmup_s;  // smoke only: overrides the manifest timing for a self-check
    std::optional<double> duration_s;
    std::optional<int> repeats;
    std::string path;                // render_stress: indexed, indirect, or meshlets; empty runs the scene's default path
    bool disable_mesh_shaders = false;  // render_stress: force the device's MeshShaders feature off (the fallback proof)
};

struct ParseResult {
    Options options;
    std::string error;               // empty on success
};

[[nodiscard]] ParseResult parse_command_line(const std::vector<std::string_view>& args);

[[nodiscard]] std::string usage_text();

}  // namespace engine::bench
