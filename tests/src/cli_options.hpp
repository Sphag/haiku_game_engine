#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace engine::tests {

enum class Suite {
    All,          // every CPU test; GPU tests are excluded so CPU gates never open a GPU
    Unit,
    Integration,
    Gpu,          // GPU tests (tagged gpu); needs --backend
    Editor,       // editor shell tests (tagged editor); --backend adds that backend's GPU editor tests
};

struct CliOptions {
    bool show_help = false;
    Suite suite = Suite::All;
    std::string backend;  // dx12 or vulkan; required for the gpu suite, empty otherwise
};

struct CliParseResult {
    CliOptions options;
    std::string error;  // empty when the command line is valid
};

// Parses arguments after the program name. Stops at the first error.
[[nodiscard]] CliParseResult parse_command_line(const std::vector<std::string_view>& args);

// Catch2 tag expression selecting a suite. The GPU suite also selects its backend tag, so `[gpu][dx12]` means DX12 tests only.
[[nodiscard]] std::string suite_tag_expression(Suite suite, std::string_view backend);

[[nodiscard]] std::string usage_text();

}  // namespace engine::tests
