#pragma once

// Build Game (US-073), headless: validation, dependency closure, cooking, packaging, and a launch check from a clean copy. The
// package is one folder for the host platform (Windows or Linux, native builds only):
//
//   <name>/
//     game_runtime(.exe)       the standalone runtime (US-060), built for this host
//     data/game.pack          the start scene's pack: its closure of cooked assets
//     shaders/                the compiled shaders of the engine build
//     NOTICES.txt             third-party notices and licenses
//     run_game.bat | run_game.sh   the launcher; it passes the pack, start scene, backend, and profile
//     game.json               the package manifest: format, settings, and every file with its size and SHA-256
//
// The build writes into <output>/<name>.build (cooked files and the verify copy) and assembles the package in a staging folder
// beside the target. The target is replaced only after the staged package has launched from a clean copy. A cancelled or failed
// build leaves no staging folder behind. An existing folder at the target is replaced only when it is a package.

#include "engine/assets/asset_project.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"

#include "asset_converter/importer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr std::string_view kPackageFormat = "engine.package";
inline constexpr int kPackageVersion = 1;
inline constexpr std::string_view kPackageManifest = "game.json";

enum class BuildStage : std::uint8_t {
    Idle,      // no build has run
    Validate,  // the converter checks the project
    Cook,      // the converter cooks every asset (reused when cached)
    Pack,      // the start scene's closure is packed
    Package,   // the staging folder is assembled
    Verify,    // the package launches from a clean copy
    Done,
};
[[nodiscard]] std::string_view to_string(BuildStage stage) noexcept;

// Shared between the caller and the worker. The caller sets cancel and reads the rest; the worker writes stage, done, and total.
struct BuildProgress {
    std::atomic<bool> cancel{false};
    std::atomic<std::uint8_t> stage{static_cast<std::uint8_t>(BuildStage::Idle)};
    std::atomic<std::size_t> done{0};   // assets converted, cached, or failed so far in Cook
    std::atomic<std::size_t> total{0};  // assets to cook
};

// The paths are UTF-8 text, checked when the build starts, so the options hold no path objects and need no defaults.
struct GameBuildOptions {
    std::string project;
    std::string output_dir;    // the folder that receives the package
    std::string game_name;     // the package folder name: letters, digits, '-', '_', '.', and not "." or ".."
    core::Guid start_scene;    // the registered scene the game starts with
    std::string backend;       // dx12 or vulkan; the runtime's default for this host when empty
    std::string profile;       // native or full; native when empty
    std::string runtime_exe;   // the game_runtime built for this host
    std::string shader_dir;    // the compiled shaders, copied into shaders/
    std::string notices_file;  // the notices text, copied as NOTICES.txt
    std::string font_file;     // the font the game UI draws with, copied as fonts/ui.ttf; empty ships none
    bool verify = true;        // launch the staged package from a clean copy before it is published
};

struct GameBuildReport {
    std::string package_dir;
    std::size_t files = 0;         // files in the package, the manifest excluded
    std::size_t pack_entries = 0;  // assets in the start scene's pack
    bool verified = false;         // the package launched from a clean copy and reported its start scene
    std::vector<std::string> log;  // one line per completed step
};

// Builds the package. A failure is returned with a message that names the step; a cancel returns "cancelled" in the message.
[[nodiscard]] core::Result<GameBuildReport, std::string> build_game(const GameBuildOptions& options, BuildProgress& progress,
                                                                    const converter::ImporterRegistry& importers);

// Runs a build on a worker thread for the editor. One build runs at a time. The destructor cancels a running build and waits for it.
class GameBuilder {
public:
    GameBuilder() = default;
    ~GameBuilder();
    GameBuilder(const GameBuilder&) = delete;
    GameBuilder& operator=(const GameBuilder&) = delete;
    GameBuilder(GameBuilder&&) = delete;
    GameBuilder& operator=(GameBuilder&&) = delete;

    // Fails when a build is already running. The options are copied to the worker.
    [[nodiscard]] core::Result<core::Unit, std::string> start(GameBuildOptions options);
    void cancel() noexcept;
    [[nodiscard]] bool running() const noexcept {
        return future_.valid();
    }
    [[nodiscard]] BuildStage stage() const noexcept {
        return static_cast<BuildStage>(progress_.stage.load());
    }
    [[nodiscard]] std::size_t done() const noexcept {
        return progress_.done.load();
    }
    [[nodiscard]] std::size_t total() const noexcept {
        return progress_.total.load();
    }
    // The result of a finished build, once. Returns nothing while the build runs or when none was started.
    [[nodiscard]] std::optional<core::Result<GameBuildReport, std::string>> poll();

private:
    BuildProgress progress_;
    std::future<core::Result<GameBuildReport, std::string>> future_;
};

}  // namespace engine::editor
