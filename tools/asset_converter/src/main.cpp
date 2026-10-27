// asset_converter: headless import, cook, and validate for a project (US-019).
//
// Exit codes: 0 when every asset passed, 1 when the report has errors (see the report or the diagnostics on stderr), and 2 for
// a usage problem or a report that cannot be written. `watch` converts the project and then again on every source change
// (US-025); its exit code describes the last pass.

#include "asset_converter/converter.hpp"
#include "asset_converter/watch.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using engine::converter::Command;
using engine::converter::Options;
using engine::converter::Report;
using engine::core::Path;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

std::string usage_text() {
    return "Usage: asset_converter <validate|import|cook> --project <dir> [--output <dir>] [--jobs <n>] [--report <file|->]\n"
           "\n"
           "Commands:\n"
           "  validate  scan the project; check sidecars, importers, settings, and dependencies. Writes nothing.\n"
           "  import    run each importer into <output>/<guid>.imported, reusing cached results.\n"
           "  cook      import what is stale and write <output>/<guid>.cooked, reusing cached results.\n"
           "  watch     cook, then cook again when the sources change, until stopped. Needs --output.\n"
           "\n"
           "Options:\n"
           "  --project <dir>    project root that holds the sources and their .meta sidecars (required)\n"
           "  --output <dir>     output directory (required for import, cook, and watch)\n"
           "  --jobs <n>         parallel workers; 0 or absent uses the hardware thread count\n"
           "  --report <file>    write the machine-readable JSON report; '-' writes it to stdout. watch rewrites it each pass\n"
           "  --max-passes <n>   watch only: stop after n passes, the first one included\n"
           "  -h, --help         show this help\n"
           "\n"
           "Exit codes: 0 ok, 1 errors in the report, 2 usage error.\n";
}

struct Arguments {
    bool show_help = false;
    std::optional<Command> command;
    std::optional<std::string> project;
    std::optional<std::string> output;
    std::optional<std::string> report;
    std::size_t jobs = 0;
    bool watch = false;
    std::optional<std::size_t> max_passes;
};

// Parses the arguments after the program name. On failure, returns the message for the user.
std::optional<std::string> parse_arguments(const std::vector<std::string_view>& args, Arguments& parsed) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            parsed.show_help = true;
            return std::nullopt;
        }
        if (arg == "validate" || arg == "import" || arg == "cook" || arg == "watch") {
            if (parsed.command || parsed.watch) {
                return "more than one command given";
            }
            if (arg == "watch") {
                parsed.watch = true;
                parsed.command = Command::Cook;  // the first pass of a watch is an ordinary cook
            } else {
                parsed.command = arg == "validate" ? Command::Validate : arg == "import" ? Command::Import : Command::Cook;
            }
            continue;
        }
        if (arg == "--project" || arg == "--output" || arg == "--report" || arg == "--jobs" || arg == "--max-passes") {
            if (i + 1 >= args.size()) {
                return std::string(arg) + " requires a value";
            }
            const std::string value(args[++i]);
            if (arg == "--project") {
                parsed.project = value;
            } else if (arg == "--output") {
                parsed.output = value;
            } else if (arg == "--report") {
                parsed.report = value;
            } else if (arg == "--jobs") {
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed.jobs);
                if (error != std::errc{} || end != value.data() + value.size()) {
                    return "--jobs requires a whole number, got '" + value + "'";
                }
            } else {
                std::size_t passes = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), passes);
                if (error != std::errc{} || end != value.data() + value.size() || passes == 0) {
                    return "--max-passes requires a whole number of at least 1, got '" + value + "'";
                }
                parsed.max_passes = passes;
            }
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-') {
            return "unknown argument: " + std::string(arg);
        }
        return "unknown command '" + std::string(arg) + "'";
    }
    if (!parsed.command) {
        return "a command is required: validate, import, cook, or watch";
    }
    if (parsed.max_passes && !parsed.watch) {
        return "--max-passes applies to watch only";
    }
    if (!parsed.project) {
        return "--project is required";
    }
    return std::nullopt;
}

void print_usage_error(const std::string& message) {
    std::fprintf(stderr, "asset_converter: %s\n\n%s", message.c_str(), usage_text().c_str());
}

void print_summary(const Report& report) {
    std::size_t counts[6] = {};
    for (const auto& outcome : report.assets) {
        ++counts[static_cast<std::size_t>(outcome.status)];
    }
    std::fprintf(stderr, "asset_converter %s: %zu assets: %zu valid, %zu imported, %zu cooked, %zu up to date, %zu failed, %zu blocked\n",
                 std::string(engine::converter::to_string(report.command)).c_str(), report.assets.size(), counts[0], counts[1],
                 counts[2], counts[3], counts[4], counts[5]);
    for (const auto& diagnostic : report.diagnostics) {
        std::fprintf(stderr, "%s %s", std::string(engine::converter::to_string(diagnostic.severity)).c_str(),
                     std::string(engine::converter::to_string(diagnostic.code)).c_str());
        if (!diagnostic.asset.empty()) {
            std::fprintf(stderr, " %s", diagnostic.asset.c_str());
        }
        if (!diagnostic.source.empty()) {
            std::fprintf(stderr, " [%s]", diagnostic.source.c_str());
        }
        std::fprintf(stderr, ": %s\n", diagnostic.message.c_str());
    }
}

// Returns false when the report could not be written.
bool write_report(const std::string& destination, const std::string& json) {
    if (destination == "-") {
        std::fwrite(json.data(), 1, json.size(), stdout);
        return true;
    }
    auto path = Path::from_utf8(destination);
    if (!path.ok()) {
        return false;
    }
    auto written = engine::core::write_file_atomic(path.value(), json);
    if (!written.ok()) {
        std::fprintf(stderr, "asset_converter: cannot write report: %s\n", engine::core::describe(written.error()).c_str());
        return false;
    }
    return true;
}

// Converts the project, then converts it again on each source change, until max_passes passes have run (never, when absent).
// Returns the exit code of the last pass, or 2 when the watch itself cannot run.
int run_watch(const Options& options, engine::converter::ImporterRegistry importers, std::optional<std::size_t> max_passes,
              const std::optional<std::string>& report_destination) {
    if (!options.output) {
        print_usage_error("watch needs an output directory");
        return kExitUsage;
    }
    auto source = engine::converter::open_directory_watch(options.project);
    if (!source.ok()) {
        std::fprintf(stderr, "asset_converter: %s\n", source.error().c_str());
        return kExitUsage;
    }
    std::fprintf(stderr, "asset_converter watch: watching %s\n", options.project.str().c_str());

    engine::converter::WatchSession session(options, std::move(importers), *source.value());
    std::size_t passes = 0;
    int exit_code = kExitOk;
    // A pass is reported the same way for every command: summary and diagnostics on stderr, and the report if asked for.
    auto deliver = [&](const Report& report) {
        ++passes;
        print_summary(report);
        exit_code = report.ok() ? kExitOk : kExitFailed;
        if (report_destination) {
            return write_report(*report_destination, engine::converter::report_to_json(report));
        }
        return true;
    };

    auto first = session.start();
    if (!first.ok()) {
        print_usage_error(first.error());
        return kExitUsage;
    }
    if (!deliver(first.value())) {
        return kExitUsage;
    }
    while (!max_passes || passes < *max_passes) {
        auto next = session.step(std::chrono::milliseconds(200));
        if (!next.ok()) {
            print_usage_error(next.error());
            return kExitUsage;
        }
        if (next.value() && !deliver(*next.value())) {
            return kExitUsage;
        }
    }
    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    Arguments parsed;
    if (auto problem = parse_arguments(args, parsed)) {
        print_usage_error(*problem);
        return kExitUsage;
    }
    if (parsed.show_help) {
        std::fputs(usage_text().c_str(), stdout);
        return kExitOk;
    }

    auto project = Path::from_utf8(*parsed.project);
    if (!project.ok()) {
        print_usage_error("--project is not valid UTF-8");
        return kExitUsage;
    }
    Options options{project.value(), std::nullopt, parsed.jobs};
    if (parsed.output) {
        auto output = Path::from_utf8(*parsed.output);
        if (!output.ok()) {
            print_usage_error("--output is not valid UTF-8");
            return kExitUsage;
        }
        options.output = output.value();
    }

    engine::converter::ImporterRegistry importers = engine::converter::ImporterRegistry::builtin();
    if (parsed.watch) {
        return run_watch(options, std::move(importers), parsed.max_passes, parsed.report);
    }
    auto result = engine::converter::run(*parsed.command, options, importers);
    if (!result.ok()) {
        print_usage_error(result.error());
        return kExitUsage;
    }
    const Report& report = result.value();

    if (parsed.report) {
        const std::string json = engine::converter::report_to_json(report);
        if (!write_report(*parsed.report, json)) {
            return kExitUsage;
        }
    }
    print_summary(report);
    return report.ok() ? kExitOk : kExitFailed;
}
