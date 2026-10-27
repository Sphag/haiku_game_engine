// game_runtime (US-060): the standalone game. It opens the resource packs it was given, checks each one, and starts the start
// scene from the cooked payload. It reads cooked files only, through the pack reader and the cooked container. Nothing here
// touches the source project, the build tree, or the converter, so the executable runs from any working directory.

#include "engine/assets/cooked_container.hpp"
#include "engine/core/path.hpp"
#include "engine/gameplay/components.hpp"
#include "engine/gameplay/game_world.hpp"
#include "engine/pack/pack.hpp"
#include "engine/runtime/lifecycle.hpp"
#include "engine/runtime/runtime_options.hpp"
#include "engine/runtime/windowed_game.hpp"
#include "engine/scene/reflection.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_format.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using engine::core::Guid;
using engine::core::Path;
using engine::pack::PackReader;

constexpr int kExitOk = 0;
constexpr int kExitLoadFailed = 1;
constexpr int kExitUsage = 2;

int load_failed(const std::string& message) {
    std::fprintf(stderr, "game_runtime: %s\n", message.c_str());
    return kExitLoadFailed;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    auto parsed = engine::runtime::parse_runtime_options(args);
    if (!parsed.ok()) {
        std::fprintf(stderr, "game_runtime: %s\n\n%s", parsed.error().c_str(), engine::runtime::runtime_usage_text().c_str());
        return kExitUsage;
    }
    const auto& options = parsed.value();
    if (options.help) {
        std::fputs(engine::runtime::runtime_usage_text().c_str(), stdout);
        return kExitOk;
    }

    // Open and verify every pack before the scene is touched. A pack that fails its checks stops the start.
    std::vector<PackReader> packs;
    std::unordered_set<Guid> known_assets;
    for (const auto& file : options.packs) {
        auto path = Path::from_utf8(file);
        if (!path.ok()) {
            return load_failed("pack path is not valid UTF-8: " + file);
        }
        auto reader = engine::pack::open_pack_file(path.value());
        if (!reader.ok()) {
            return load_failed(file + ": " + std::string(engine::pack::to_string(reader.error().code)) + ": " +
                               reader.error().message);
        }
        for (const auto& entry : reader.value().entries()) {
            if (!known_assets.insert(entry.guid).second) {
                return load_failed(file + ": asset " + entry.guid.to_string() + " is in more than one pack");
            }
        }
        packs.push_back(std::move(reader).value());
    }

    // The start scene must be a root of a pack, so the scene is the one the build chose and not a stray dependency.
    const PackReader* holder = nullptr;
    for (const auto& pack : packs) {
        for (const auto& root : pack.roots()) {
            if (root == options.start_scene) {
                holder = &pack;
            }
        }
    }
    if (holder == nullptr) {
        return load_failed("start scene " + options.start_scene.to_string() + " is not a root of any pack");
    }
    const auto* entry = holder->find(options.start_scene);

    auto text = engine::assets::read_cooked_container(holder->payload(*entry));
    if (!text.ok()) {
        return load_failed("start scene " + options.start_scene.to_string() + ": " + text.error());
    }

    // The built-in components are registered, so a scene authored in the editor loads. A scene that names a type the catalog does
    // not hold is rejected here, not loaded partly. The catalog is the place a game adds its own types.
    engine::scene::ComponentCatalog catalog;
    if (!engine::gameplay::register_builtin_components(catalog).ok()) {
        return load_failed("the built-in components could not be registered");
    }
    auto document = engine::scene::parse_scene(text.value(), catalog);
    if (!document.ok()) {
        return load_failed("start scene " + options.start_scene.to_string() + ": " + document.error().message);
    }
    // A scene may instantiate prefabs, which the packs carry as assets: the library holds every one of them before the scene loads.
    engine::scene::PrefabLibrary prefabs(catalog);
    for (const auto& pack : packs) {
        for (const auto& pack_entry : pack.entries()) {
            auto cooked = engine::assets::read_cooked_container(pack.payload(pack_entry));
            if (!cooked.ok() || cooked.value().find("\"engine.prefab\"") == std::string::npos) {
                continue;
            }
            auto prefab = engine::scene::parse_prefab(cooked.value(), catalog);
            if (!prefab.ok()) {
                return load_failed("prefab " + pack_entry.guid.to_string() + ": " + prefab.error().message);
            }
            if (!prefabs.add(pack_entry.guid, std::move(prefab).value()).ok()) {
                return load_failed("prefab " + pack_entry.guid.to_string() + " could not be added to the library");
            }
        }
    }
    engine::scene::Scene scene(catalog, prefabs);
    auto loaded = scene.load(document.value(), known_assets);
    if (!loaded.ok()) {
        return load_failed("start scene " + options.start_scene.to_string() + ": " + loaded.error().message);
    }
    const auto missing = scene.missing_references();
    if (!missing.empty()) {
        return load_failed("start scene " + options.start_scene.to_string() + " refers to an asset that is not in the packs: " +
                           missing.front().asset.to_string());
    }

    // Scripts come out of the packs by GUID. A script that is not in a pack is reported by the world, and the others still run.
    const auto read_script = [&packs](const Guid& guid) -> engine::core::Result<std::string, std::string> {
        for (const auto& pack : packs) {
            if (const auto* script_entry = pack.find(guid)) {
                auto cooked = engine::assets::read_cooked_container(pack.payload(*script_entry));
                if (!cooked.ok()) {
                    return engine::core::failure(cooked.error());
                }
                return engine::core::success(std::string(cooked.value()));
            }
        }
        return engine::core::failure(std::string("not in any pack"));
    };

    engine::runtime::Lifecycle lifecycle;
    if (!lifecycle.start().ok()) {
        return load_failed("runtime lifecycle could not start");
    }
    std::printf("game_runtime: started scene %s entities=%zu packs=%zu backend=%s profile=%s\n",
                options.start_scene.to_string().c_str(), scene.to_document().entities.size(), packs.size(),
                options.backend.c_str(), options.profile.c_str());

    // The game runs on the CPU: physics bodies and Lua scripts from the scene's components, stepped at 60 Hz for the requested frames.
    // Drawing needs a window and a device, which this build's runtime does not open yet (docs/components.md).
    auto world = engine::gameplay::GameWorld::create(scene, read_script);
    if (!world.ok()) {
        return load_failed("the game world could not start: " + world.error());
    }
    int exit_code = kExitOk;
    const auto report_errors = [&exit_code, &world]() {
        for (const std::string& error : world.value()->take_errors()) {
            std::fprintf(stderr, "game_runtime: %s\n", error.c_str());
            exit_code = kExitLoadFailed;
        }
    };
    report_errors();
    if (options.window) {
        // Shaders ship in a folder beside the executable (Build Game copies them there).
        const std::string shader_dir = (std::filesystem::absolute(std::filesystem::path(argv[0])).parent_path() / "shaders").string();
        const int windowed = engine::runtime::run_windowed_game(scene, *world.value(), options, shader_dir);
        world.value().reset();
        if (windowed != kExitOk) {
            return windowed;
        }
        if (!lifecycle.stop().ok()) {
            return load_failed("runtime lifecycle could not stop");
        }
        std::printf("game_runtime: stopped\n");
        return exit_code;
    }
    for (std::uint32_t frame = 0; frame < options.frames; ++frame) {
        world.value()->advance(1.0 / 60.0);
    }
    report_errors();
    const auto stats = world.value()->stats();
    std::printf("game_runtime: simulated %u frames bodies=%zu scripts=%zu fixed_steps=%llu\n", options.frames, stats.bodies,
                stats.scripts, static_cast<unsigned long long>(stats.fixed_steps));
    world.value().reset();
    if (exit_code != kExitOk) {
        return exit_code;
    }

    if (!lifecycle.stop().ok()) {
        return load_failed("runtime lifecycle could not stop");
    }
    std::printf("game_runtime: stopped\n");
    return kExitOk;
}
