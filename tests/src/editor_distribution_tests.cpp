// Build Game (US-073), headless: the package layout, the launch check from a clean folder, the refusals (an unknown or non-scene start,
// a bad name, a folder that is not a package), cancellation that leaves no staging folder, and a rebuild that replaces the package.
// The launch check starts the standalone runtime that the build compiled (ENGINE_TEST_GAME_RUNTIME), so these cases run on the host.

#include "engine/editor/distribution.hpp"

#include "engine/assets/content_hash.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include "asset_converter/importer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using engine::core::Guid;
using engine::core::Path;
using engine::editor::BuildProgress;
using engine::editor::GameBuildOptions;
using engine::editor::build_game;

constexpr const char* kStartScene = "3898c084-2f9e-4e6f-af6c-4ae6e8920395";
constexpr const char* kPaletteBlob = "7c234218-2288-45ea-a06d-833e300036a3";

// Removes the scratch folder when the case ends, even when a check failed part way through.
struct Scratch {
    fs::path dir;
    explicit Scratch(const std::string& tag) : dir(fs::temp_directory_path() / ("engine_distribution_" + tag)) {
        fs::remove_all(dir);
        fs::create_directories(dir / "output");
        fs::create_directories(dir / "shaders");
        std::ofstream(dir / "shaders" / "test.spv", std::ios::binary) << "shader bytes";
        std::ofstream(dir / "NOTICES.txt", std::ios::binary) << "notices";
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

std::string path_of(const fs::path& p) {
    return p.string();
}

GameBuildOptions options_for(const Scratch& scratch, const std::string& name) {
    GameBuildOptions options;
    options.project = ENGINE_TEST_PACK_PROJECT;
    options.output_dir = path_of(scratch.dir / "output");
    options.game_name = name;
    options.start_scene = Guid::parse(kStartScene).value();
    options.backend = "vulkan";
    options.profile = "native";
    options.runtime_exe = ENGINE_TEST_GAME_RUNTIME;
    options.shader_dir = path_of(scratch.dir / "shaders");
    options.notices_file = path_of(scratch.dir / "NOTICES.txt");
    return options;
}

std::string read_text(const fs::path& p) {
    auto text = engine::core::read_file(Path::from_utf8(p.string()).value());
    return text.ok() ? text.value() : std::string();
}

}  // namespace

TEST_CASE("Build Game writes a package that launches from a clean folder", "[editor][integration]") {
    Scratch scratch("package");
    const auto importers = engine::converter::ImporterRegistry::builtin();
    BuildProgress progress;
    const auto result = build_game(options_for(scratch, "Sample"), progress, importers);
    INFO("build_game error: " << (result.ok() ? std::string() : result.error()));
    REQUIRE(result.ok());
    CHECK(result.value().verified);
    CHECK(result.value().pack_entries == 2);

    const fs::path package = scratch.dir / "output" / "Sample";
    const std::string runtime_name = std::string(ENGINE_TEST_GAME_RUNTIME).substr(std::string(ENGINE_TEST_GAME_RUNTIME).find_last_of("/\\") + 1);
    CHECK(fs::exists(package / runtime_name));
    CHECK(fs::exists(package / "data" / "game.pack"));
    CHECK(fs::exists(package / "shaders" / "test.spv"));
    CHECK(fs::exists(package / "NOTICES.txt"));
    CHECK(fs::exists(package / "game.json"));
#ifdef _WIN32
    CHECK(fs::exists(package / "run_game.bat"));
#else
    CHECK(fs::exists(package / "run_game.sh"));
#endif

    const std::string manifest = read_text(package / "game.json");
    CHECK(manifest.find("\"format\": \"engine.package\"") != std::string::npos);
    CHECK(manifest.find("\"start_scene\": \"" + std::string(kStartScene) + "\"") != std::string::npos);
    const std::string pack_bytes = read_text(package / "data" / "game.pack");
    const std::string pack_hash = engine::assets::ContentHash::of(pack_bytes).to_hex();
    CHECK(manifest.find(pack_hash) != std::string::npos);

    const std::string launcher = read_text(
#ifdef _WIN32
        package / "run_game.bat"
#else
        package / "run_game.sh"
#endif
    );
    CHECK(launcher.find(std::string("--start-scene ") + kStartScene) != std::string::npos);

    // Nothing is left beside the package: the staging folder is gone and the verify copy is removed.
    CHECK_FALSE(fs::exists(scratch.dir / "output" / "Sample.staging"));
    CHECK_FALSE(fs::exists(scratch.dir / "output" / "Sample.build" / "verify"));
}

TEST_CASE("Build Game refuses a start scene that is not a registered asset", "[editor][integration]") {
    Scratch scratch("unknown_start");
    const auto importers = engine::converter::ImporterRegistry::builtin();
    BuildProgress progress;
    auto options = options_for(scratch, "Sample");
    options.start_scene = Guid::parse("11111111-2222-4333-8444-555555555555").value();
    const auto result = build_game(options, progress, importers);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("is not a registered asset") != std::string::npos);
    CHECK_FALSE(fs::exists(scratch.dir / "output" / "Sample"));
}

TEST_CASE("Build Game refuses a start asset that is not a scene", "[editor][integration]") {
    Scratch scratch("not_scene");
    const auto importers = engine::converter::ImporterRegistry::builtin();
    BuildProgress progress;
    auto options = options_for(scratch, "Sample");
    options.start_scene = Guid::parse(kPaletteBlob).value();
    const auto result = build_game(options, progress, importers);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("is not a scene") != std::string::npos);
}

TEST_CASE("Build Game refuses a package name that could escape the output folder", "[editor][integration]") {
    Scratch scratch("bad_name");
    const auto importers = engine::converter::ImporterRegistry::builtin();
    BuildProgress progress;
    for (const std::string& name : {std::string(".."), std::string("a/b"), std::string(""), std::string(".hidden")}) {
        const auto result = build_game(options_for(scratch, name), progress, importers);
        REQUIRE_FALSE(result.ok());
        CHECK(result.error().find("game name") != std::string::npos);
    }
}

TEST_CASE("Build Game does not replace a folder that is not a game package", "[editor][integration]") {
    Scratch scratch("not_package");
    const auto importers = engine::converter::ImporterRegistry::builtin();
    BuildProgress progress;
    const fs::path existing = scratch.dir / "output" / "Sample";
    fs::create_directories(existing);
    std::ofstream(existing / "notes.txt", std::ios::binary) << "keep me";

    const auto result = build_game(options_for(scratch, "Sample"), progress, importers);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().find("is not a game package") != std::string::npos);
    CHECK(read_text(existing / "notes.txt") == "keep me");
}

TEST_CASE("A cancelled Build Game leaves no package and no staging folder", "[editor][integration]") {
    Scratch scratch("cancel");
    const auto importers = engine::converter::ImporterRegistry::builtin();
    BuildProgress progress;
    progress.cancel.store(true);
    const auto result = build_game(options_for(scratch, "Sample"), progress, importers);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error() == "cancelled");
    CHECK_FALSE(fs::exists(scratch.dir / "output" / "Sample"));
    CHECK_FALSE(fs::exists(scratch.dir / "output" / "Sample.staging"));
}

TEST_CASE("Rebuilding a game replaces the previous package", "[editor][integration]") {
    Scratch scratch("rebuild");
    const auto importers = engine::converter::ImporterRegistry::builtin();
    BuildProgress progress;
    auto options = options_for(scratch, "Sample");
    options.verify = false;
    REQUIRE(build_game(options, progress, importers).ok());
    std::ofstream(scratch.dir / "output" / "Sample" / "stale.txt", std::ios::binary) << "old";

    const auto second = build_game(options, progress, importers);
    REQUIRE(second.ok());
    CHECK_FALSE(second.value().verified);
    CHECK_FALSE(fs::exists(scratch.dir / "output" / "Sample" / "stale.txt"));
    CHECK(fs::exists(scratch.dir / "output" / "Sample" / "data" / "game.pack"));
}
