#include "engine/benchmark/cli.hpp"

#include <cstdlib>
#include <initializer_list>

namespace engine::bench {
namespace {

bool is_one_of(std::string_view value, std::initializer_list<std::string_view> allowed) {
    for (const auto candidate : allowed) {
        if (value == candidate) {
            return true;
        }
    }
    return false;
}

// Reads the whole text as a number, so "4x" and "" are refused rather than read as a prefix.
bool parse_double(std::string_view text, double& out) {
    const std::string copy(text);
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return !copy.empty() && end == copy.c_str() + copy.size();
}

bool parse_int(std::string_view text, int& out) {
    const std::string copy(text);
    char* end = nullptr;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (copy.empty() || end != copy.c_str() + copy.size() || value < 1 || value > 1000) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

}  // namespace

std::string usage_text() {
    return "Usage: engine_bench --suite <smoke|acceptance|streaming_soak> [--backend <dx12|vulkan>] --output <dir> [options]\n"
           "\n"
           "Runs the benchmark suite named in benchmarks/manifest.json and writes report.json plus one raw CSV per repeat.\n"
           "\n"
           "Suites:\n"
           "  smoke       a CPU self-check of the harness. It needs no GPU and applies no performance contract.\n"
           "  acceptance  the PRD performance contract: every scene and profile on the named backend, with the fixed timing\n"
           "              (30 s warmup, 180 s measured, 3 repeats). Run it on a native OS/backend pair in a Release build.\n"
           "\n"
           "Options:\n"
           "  --suite <name>      smoke, acceptance, or streaming_soak (required)\n"
           "  --backend <name>    dx12 or vulkan; required for acceptance\n"
           "  --output <dir>      the report folder, for example artifacts/bench/<run-id> (required)\n"
           "  --manifest <file>   the manifest to use (default: benchmarks/manifest.json in the repository)\n"
           "  --warmup <s>        smoke only: overrides the manifest warmup\n"
           "  --duration <s>      smoke only: overrides the manifest duration\n"
           "  --repeats <n>       smoke only: overrides the manifest repeat count\n"
           "  --path <name>       render_stress: indexed, indirect, or meshlets (the draw path for the run)\n"
           "  --disable-mesh-shaders  render_stress: turn the MeshShaders feature off; meshlets then fall back and say so\n"
           "  -h, --help          show this help\n"
           "\n"
           "Exit codes: 0 every run passed, 1 a performance threshold failed, 2 usage or manifest error,\n"
           "3 a run was unsupported, missing, or incomplete (not verified), including an empty run list.\n";
}

ParseResult parse_command_line(const std::vector<std::string_view>& args) {
    ParseResult result;
    Options& options = result.options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.show_help = true;
            return result;
        }
        if (arg == "--disable-mesh-shaders") {
            options.disable_mesh_shaders = true;
            continue;
        }
        const bool takes_value = arg == "--suite" || arg == "--backend" || arg == "--output" || arg == "--manifest" ||
                                 arg == "--warmup" || arg == "--duration" || arg == "--repeats" ||
                                 arg == "--path";
        if (!takes_value) {
            result.error = "unknown argument: " + std::string(arg);
            return result;
        }
        if (i + 1 >= args.size()) {
            result.error = std::string(arg) + " requires a value";
            return result;
        }
        const std::string_view value = args[++i];
        double seconds = 0.0;
        int repeats = 0;
        if (arg == "--suite") {
            options.suite = std::string(value);
        } else if (arg == "--backend") {
            options.backend = std::string(value);
        } else if (arg == "--output") {
            options.output = std::string(value);
        } else if (arg == "--manifest") {
            options.manifest = std::string(value);
        } else if (arg == "--path") {
            if (!is_one_of(value, {"indexed", "indirect", "meshlets"})) {
                result.error = "invalid path '" + std::string(value) + "' (indexed, indirect, or meshlets)";
                return result;
            }
            options.path = std::string(value);
        } else if (arg == "--warmup") {
            if (!parse_double(value, seconds) || seconds < 0.0) {
                result.error = "--warmup must be a number of seconds >= 0";
                return result;
            }
            options.warmup_s = seconds;
        } else if (arg == "--duration") {
            if (!parse_double(value, seconds) || seconds <= 0.0) {
                result.error = "--duration must be a number of seconds > 0";
                return result;
            }
            options.duration_s = seconds;
        } else if (arg == "--repeats") {
            if (!parse_int(value, repeats)) {
                result.error = "--repeats must be an integer from 1 to 1000";
                return result;
            }
            options.repeats = repeats;
        }
    }

    if (options.suite.empty()) {
        result.error = "--suite requires a value (smoke, acceptance, or streaming_soak)";
        return result;
    }
    if (!is_one_of(options.suite, {"smoke", "acceptance", "streaming_soak"})) {
        result.error = "invalid suite '" + options.suite + "'";
        return result;
    }
    if (!options.backend.empty() && !is_one_of(options.backend, {"dx12", "vulkan"})) {
        result.error = "invalid backend '" + options.backend + "'";
        return result;
    }
    if (options.suite == "acceptance" && options.backend.empty()) {
        result.error = "--suite acceptance requires --backend dx12 or vulkan";
        return result;
    }
    if (options.suite == "smoke" && !options.backend.empty()) {
        result.error = "--suite smoke takes no --backend; it runs on the CPU";
        return result;
    }
    if (options.suite == "acceptance" && (options.warmup_s || options.duration_s || options.repeats)) {
        result.error = "the acceptance timing is fixed by the performance contract; --warmup, --duration, and --repeats are smoke only";
        return result;
    }
    if (options.output.empty()) {
        result.error = "--output requires a folder";
        return result;
    }
    return result;
}

}  // namespace engine::bench
