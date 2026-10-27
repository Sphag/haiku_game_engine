// engine_workload (US-075): generates the seeded benchmark workloads from the benchmark table and writes them, or checks them
// against the expected file. Generation reads only the manifest and the repository; it downloads nothing and needs no
// private assets. Exit codes: 0 written or every scene matches, 1 a scene differs from the expected file, 2 usage or input error.

#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/workload.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::bench::Workload;

constexpr int kExitMismatch = 1;
constexpr int kExitUsage = 2;

std::string usage_text() {
    return "Usage: engine_workload [--manifest <file>] (--write <file> | --check <file>)\n"
           "\n"
           "Generates every generated scene (the non-builtin scenes of benchmarks/manifest.json) from its seed and table counts.\n"
           "\n"
           "  --write <file>     write the expected counts and digests (benchmarks/workload_expected.json)\n"
           "  --check <file>     regenerate and compare with an expected file; each difference is printed\n"
           "  --manifest <file>  the benchmark table to use (default: benchmarks/manifest.json in the repository)\n"
           "  -h, --help         show this help\n"
           "\n"
           "Exit codes: 0 written or every scene matches, 1 a scene differs, 2 usage or input error.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string manifest_path = ENGINE_BENCH_MANIFEST;
    std::string write_path;
    std::string check_path;
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << usage_text();
            return 0;
        }
        if (arg != "--manifest" && arg != "--write" && arg != "--check") {
            std::cerr << "engine_workload: unknown argument: " << arg << "\n\n" << usage_text();
            return kExitUsage;
        }
        if (i + 1 >= args.size()) {
            std::cerr << "engine_workload: " << arg << " requires a value\n";
            return kExitUsage;
        }
        const std::string value(args[++i]);
        if (arg == "--manifest") {
            manifest_path = value;
        } else if (arg == "--write") {
            write_path = value;
        } else {
            check_path = value;
        }
    }
    if (write_path.empty() == check_path.empty()) {
        std::cerr << "engine_workload: give exactly one of --write or --check\n\n" << usage_text();
        return kExitUsage;
    }

    const auto manifest_file = engine::core::Path::from_utf8(manifest_path);
    if (!manifest_file.ok()) {
        std::cerr << "engine_workload: manifest path is not valid UTF-8: " << manifest_path << "\n";
        return kExitUsage;
    }
    const auto manifest_text = engine::core::read_text_file(manifest_file.value());
    if (!manifest_text.ok()) {
        std::cerr << "engine_workload: cannot read manifest " << manifest_path << ": "
                  << engine::core::describe(manifest_text.error()) << "\n";
        return kExitUsage;
    }
    const auto manifest = engine::bench::parse_manifest(manifest_text.value());
    if (!manifest.ok()) {
        std::cerr << "engine_workload: invalid manifest " << manifest_path << ": " << manifest.error() << "\n";
        return kExitUsage;
    }

    const auto world = engine::bench::check_generator_world(manifest.value().world_json);
    if (!world.ok()) {
        std::cerr << "engine_workload: manifest " << manifest_path << ": " << world.error() << "\n";
        return kExitUsage;
    }

    std::map<std::string, Workload> generated;
    for (const auto& [name, scene] : manifest.value().scenes) {
        if (scene.builtin) {
            continue;
        }
        auto workload = engine::bench::generate_workload(name, scene);
        if (!workload.ok()) {
            std::cerr << "engine_workload: " << workload.error() << "\n";
            return kExitUsage;
        }
        generated[name] = workload.value();
    }
    if (generated.empty()) {
        std::cerr << "engine_workload: the manifest has no generated scene\n";
        return kExitUsage;
    }

    if (!write_path.empty()) {
        const auto path = engine::core::Path::from_utf8(write_path);
        if (!path.ok()) {
            std::cerr << "engine_workload: output path is not valid UTF-8: " << write_path << "\n";
            return kExitUsage;
        }
        const auto written = engine::core::write_file_atomic(path.value(), engine::bench::workloads_json(generated));
        if (!written.ok()) {
            std::cerr << "engine_workload: cannot write " << write_path << ": " << engine::core::describe(written.error()) << "\n";
            return kExitUsage;
        }
        std::cout << "engine_workload: wrote " << generated.size() << " scenes to " << write_path << "\n";
        return 0;
    }

    const auto path = engine::core::Path::from_utf8(check_path);
    if (!path.ok()) {
        std::cerr << "engine_workload: expected path is not valid UTF-8: " << check_path << "\n";
        return kExitUsage;
    }
    const auto expected_text = engine::core::read_text_file(path.value());
    if (!expected_text.ok()) {
        std::cerr << "engine_workload: cannot read " << check_path << ": " << engine::core::describe(expected_text.error()) << "\n";
        return kExitUsage;
    }
    const auto expected = engine::bench::parse_workloads(expected_text.value());
    if (!expected.ok()) {
        std::cerr << "engine_workload: invalid expected file " << check_path << ": " << expected.error() << "\n";
        return kExitUsage;
    }

    int differences = 0;
    std::set<std::string> names;
    for (const auto& [name, workload] : expected.value()) {
        names.insert(name);
        const auto actual = generated.find(name);
        if (actual == generated.end()) {
            std::cout << name << ": in the expected file but not generated by the manifest\n";
            ++differences;
            continue;
        }
        for (const auto& diff : engine::bench::compare_workloads(workload, actual->second)) {
            std::cout << name << ": " << diff << "\n";
            ++differences;
        }
    }
    for (const auto& [name, workload] : generated) {
        (void)workload;
        if (names.count(name) == 0) {
            std::cout << name << ": generated by the manifest but missing from the expected file\n";
            ++differences;
        }
    }
    if (differences > 0) {
        std::cout << "engine_workload: " << differences << " difference(s)\n";
        return kExitMismatch;
    }
    std::cout << "engine_workload: " << generated.size() << " scenes match " << check_path << "\n";
    return 0;
}
