#include "cli_options.hpp"

namespace engine::tests {
namespace {

constexpr std::string_view kSuiteFlag = "--suite";
constexpr std::string_view kSuiteFlagWithValue = "--suite=";

constexpr std::string_view kBackendFlag = "--backend";
constexpr std::string_view kBackendFlagWithValue = "--backend=";

bool parse_suite(std::string_view value, Suite& suite) {
    if (value == "all") {
        suite = Suite::All;
    } else if (value == "unit") {
        suite = Suite::Unit;
    } else if (value == "integration") {
        suite = Suite::Integration;
    } else if (value == "gpu") {
        suite = Suite::Gpu;
    } else if (value == "editor") {
        suite = Suite::Editor;
    } else {
        return false;
    }
    return true;
}

bool is_known_backend(std::string_view value) {
    return value == "dx12" || value == "vulkan";
}

}  // namespace

CliParseResult parse_command_line(const std::vector<std::string_view>& args) {
    CliParseResult result;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        if (arg == "--help" || arg == "-h") {
            result.options.show_help = true;
            continue;
        }

        if (arg == kBackendFlag || arg.starts_with(kBackendFlagWithValue)) {
            std::string_view backend;
            if (arg == kBackendFlag) {
                if (i + 1 >= args.size()) {
                    result.error = "--backend requires a value";
                    return result;
                }
                backend = args[++i];
            } else {
                backend = arg.substr(kBackendFlagWithValue.size());
            }
            if (!is_known_backend(backend)) {
                result.error = "invalid backend '" + std::string(backend) + "' (expected dx12 or vulkan)";
                return result;
            }
            result.options.backend = std::string(backend);
            continue;
        }

        std::string_view value;
        if (arg == kSuiteFlag) {
            if (i + 1 >= args.size()) {
                result.error = "--suite requires a value";
                return result;
            }
            value = args[++i];
        } else if (arg.starts_with(kSuiteFlagWithValue)) {
            value = arg.substr(kSuiteFlagWithValue.size());
        } else {
            result.error = "unknown argument: " + std::string(arg);
            return result;
        }

        if (!parse_suite(value, result.options.suite)) {
            result.error = "invalid suite '" + std::string(value) + "' (expected unit, integration, gpu, editor, or all)";
            return result;
        }
    }

    // Cross-option rules are checked after the loop, so they do not depend on the order of the arguments.
    const bool gpu = result.options.suite == Suite::Gpu;
    const bool editor = result.options.suite == Suite::Editor;
    if (gpu && result.options.backend.empty()) {
        result.error = "--suite gpu requires --backend <dx12|vulkan>";
    } else if (!gpu && !editor && !result.options.backend.empty()) {
        result.error = "--backend applies only to --suite gpu or --suite editor";
    }
    return result;
}

std::string suite_tag_expression(Suite suite, std::string_view backend) {
    switch (suite) {
        case Suite::All:
            return "~[gpu]";
        case Suite::Unit:
            return "[unit]";
        case Suite::Integration:
            return "[integration]";
        case Suite::Gpu:
            return "[gpu][" + std::string(backend) + "]";
        case Suite::Editor:
            // The CPU editor tests run without a backend. With one, the GPU editor tests of that backend join them.
            if (backend.empty()) {
                return "[editor]~[gpu]";
            }
            return "[editor]~[gpu],[editor][gpu][" + std::string(backend) + "]";
    }
    return "~[gpu]";
}

std::string usage_text() {
    return "Usage: engine_tests [--suite <unit|integration|gpu|editor|all>] [--backend <dx12|vulkan>] [--help]\n"
           "\n"
           "Runs the CPU test suites, or the GPU suite on one backend. CPU tests are selected by their unit or integration tag.\n"
           "GPU tests are tagged gpu and are never part of the CPU suites.\n"
           "\n"
           "Options:\n"
           "  --suite <name>     Run only unit or integration tests, the gpu or editor suite (default: all CPU tests)\n"
           "  --backend <name>   GPU backend for --suite gpu (required) or --suite editor (optional): dx12 or vulkan\n"
           "  -h, --help         Show this help and exit\n"
           "\n"
           "Exit codes:\n"
           "  0    all selected tests passed, or help was shown\n"
           "  2    invalid command line\n"
           "  3    the requested GPU backend is not available here (reported as unsupported, not passed)\n"
           "  other nonzero values report the number of failed tests\n";
}

}  // namespace engine::tests
