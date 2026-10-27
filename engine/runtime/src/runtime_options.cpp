#include "engine/runtime/runtime_options.hpp"

#include <utility>

namespace engine::runtime {

namespace {

std::string default_backend() {
#ifdef _WIN32
    return std::string(kBackendDx12);
#else
    return std::string(kBackendVulkan);
#endif
}

}  // namespace

std::string runtime_usage_text() {
    return "Usage: game_runtime --pack <file> [--pack <file>...] --start-scene <guid> [--backend dx12|vulkan] [--profile native|full]\n"
           "\n"
           "Runs a packaged game from its resource packs. The runtime reads cooked files only; it needs no source assets,\n"
           "no build tree, and no editor.\n"
           "\n"
           "Options:\n"
           "  --pack <file>        a resource pack written by asset_packer (required, repeatable)\n"
           "  --start-scene <guid> the scene to start; it must be a root of one of the packs (required)\n"
           "  --backend <name>    dx12 or vulkan (default: dx12 on Windows, vulkan elsewhere)\n"
           "  --profile <name>    native or full (default: native)\n"
           "  -h, --help          show this help\n"
           "\n"
           "Exit codes: 0 started and stopped, 1 a pack or the scene could not be loaded, 2 usage error.\n";
}

core::Result<RuntimeOptions, std::string> parse_runtime_options(const std::vector<std::string>& args) {
    RuntimeOptions options;
    options.backend = default_backend();
    bool have_start_scene = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return core::success(std::move(options));
        }
        if (arg != "--pack" && arg != "--start-scene" && arg != "--backend" && arg != "--profile") {
            return core::failure("unknown argument: " + arg);
        }
        if (i + 1 >= args.size()) {
            return core::failure(arg + " requires a value");
        }
        const std::string& value = args[++i];
        if (arg == "--pack") {
            if (value.empty()) {
                return core::failure(std::string("--pack requires a file name"));
            }
            options.packs.push_back(value);
        } else if (arg == "--start-scene") {
            auto guid = core::Guid::parse(value);
            if (!guid.ok()) {
                return core::failure("--start-scene requires a GUID, got '" + value + "'");
            }
            options.start_scene = guid.value();
            have_start_scene = true;
        } else if (arg == "--backend") {
            if (value != kBackendDx12 && value != kBackendVulkan) {
                return core::failure("invalid backend '" + value + "'");
            }
            options.backend = value;
        } else {
            if (value != kProfileNative && value != kProfileFull) {
                return core::failure("invalid profile '" + value + "'");
            }
            options.profile = value;
        }
    }

    if (options.packs.empty()) {
        return core::failure(std::string("--pack is required"));
    }
    if (!have_start_scene) {
        return core::failure(std::string("--start-scene is required"));
    }
    return core::success(std::move(options));
}

}  // namespace engine::runtime
