// asset_packer (US-060): builds a resource pack from a project's registry and the cooked files that asset_converter wrote.
// It links the pack and assets libraries only. The converter is not linked, so the packer cannot import anything: it
// packages what has already been cooked, and it fails when a reachable asset is missing.

#include "engine/assets/asset_project.hpp"
#include "engine/assets/cooked_container.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/pack/pack.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using engine::core::Guid;
using engine::core::Path;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

std::string usage_text() {
    return "Usage: asset_packer --project <dir> --cooked <dir> --root <guid> [--root <guid>...] --output <file>\n"
           "\n"
           "Packs the roots and every asset they depend on into one resource pack. The cooked directory holds the\n"
           "<guid>.cooked files that asset_converter cook wrote for the same project.\n"
           "\n"
           "Options:\n"
           "  --project <dir>   project root with the sources and their .meta sidecars (required)\n"
           "  --cooked <dir>    cooked output directory (required)\n"
           "  --root <guid>     an asset to pack; repeat for each start scene or entry point (required)\n"
           "  --output <file>   the pack file to write; an existing pack is replaced only on success (required)\n"
           "  -h, --help        show this help\n"
           "\n"
           "Exit codes: 0 packed, 1 the pack could not be built (a missing root, a missing dependency, a missing cooked\n"
           "file, or an I/O error), 2 usage error.\n";
}

void print_usage_error(const std::string& message) {
    std::fprintf(stderr, "asset_packer: %s\n\n%s", message.c_str(), usage_text().c_str());
}

struct Options {
    std::string project;
    std::string cooked;
    std::string output;
    std::vector<Guid> roots;
    bool help = false;
};

// Returns an empty string on success, or the usage message.
std::string parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return {};
        }
        if (arg != "--project" && arg != "--cooked" && arg != "--root" && arg != "--output") {
            return "unknown argument: " + arg;
        }
        if (i + 1 >= argc) {
            return arg + " requires a value";
        }
        const std::string value = argv[++i];
        if (arg == "--project") {
            options.project = value;
        } else if (arg == "--cooked") {
            options.cooked = value;
        } else if (arg == "--output") {
            options.output = value;
        } else {
            auto guid = Guid::parse(value);
            if (!guid.ok()) {
                return "--root requires a GUID, got '" + value + "'";
            }
            options.roots.push_back(guid.value());
        }
    }
    if (options.project.empty()) {
        return "--project is required";
    }
    if (options.cooked.empty()) {
        return "--cooked is required";
    }
    if (options.output.empty()) {
        return "--output is required";
    }
    if (options.roots.empty()) {
        return "at least one --root is required";
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (const std::string error = parse_options(argc, argv, options); !error.empty()) {
        print_usage_error(error);
        return kExitUsage;
    }
    if (options.help) {
        std::fputs(usage_text().c_str(), stdout);
        return kExitOk;
    }

    auto project = Path::from_utf8(options.project);
    auto cooked_dir = Path::from_utf8(options.cooked);
    auto output = Path::from_utf8(options.output);
    if (!project.ok() || !cooked_dir.ok() || !output.ok()) {
        std::fprintf(stderr, "asset_packer: a path is not valid UTF-8 or is malformed\n");
        return kExitFailed;
    }

    auto scan = engine::assets::scan_project(project.value());
    if (!scan.ok()) {
        std::fprintf(stderr, "asset_packer: project scan failed: %s\n", scan.error().message.c_str());
        return kExitFailed;
    }

    const auto cooked = [&cooked_dir](const Guid& guid) -> engine::core::Result<std::string, std::string> {
        auto path = cooked_dir.value().join(guid.to_string() + ".cooked");
        if (!path.ok()) {
            return engine::core::failure(std::string("cooked file name is not a valid path"));
        }
        return engine::assets::read_cooked_file(path.value());
    };

    auto bytes = engine::pack::build_pack(scan.value().registry, options.roots, cooked);
    if (!bytes.ok()) {
        const auto& error = bytes.error();
        const std::string code(engine::pack::to_string(error.code));
        std::fprintf(stderr, "asset_packer: %s: %s: %s\n", code.c_str(), error.guid.c_str(), error.message.c_str());
        return kExitFailed;
    }

    auto written = engine::pack::write_pack_file(output.value(), bytes.value());
    if (!written.ok()) {
        std::fprintf(stderr, "asset_packer: %s\n", written.error().message.c_str());
        return kExitFailed;
    }
    auto reader = engine::pack::PackReader::open(bytes.value());
    if (!reader.ok()) {
        std::fprintf(stderr, "asset_packer: the built pack failed its own check: %s\n", reader.error().message.c_str());
        return kExitFailed;
    }
    std::printf("packed %zu assets from %zu roots into %s (%zu bytes)\n", reader.value().entries().size(), options.roots.size(),
                output.value().str().c_str(), bytes.value().size());
    return kExitOk;
}
