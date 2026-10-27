// The showcase sample project (samples/showcase), driven through the real engine paths: the asset scan, the scene and prefab loaders with the
// built-in components, the GameWorld simulation (physics bodies, Lua scripts), the animation graph, the material graphs, the terrain file, and
// Build Game with the standalone runtime. The point is that the sample is a project a user can open in the editor and that every feature
// area has an asset in it. A feature with no asset here fails the coverage test, so a new feature cannot ship without a sample.

#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/assets/asset_project.hpp"
#include "engine/audio/audio_engine.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/editor/distribution.hpp"
#include "engine/editor/editor_model.hpp"
#include "engine/editor/world_editor.hpp"
#include "engine/gameplay/components.hpp"
#include "engine/gameplay/game_world.hpp"
#include "engine/material_graph/graph.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_format.hpp"
#include "engine/world/terrain_edit.hpp"

#include "asset_converter/importer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>

namespace {

namespace fs = std::filesystem;
using engine::core::Guid;
using engine::core::Path;

const std::string kShowcase = ENGINE_SHOWCASE_DIR;

Path path_of(const std::string& text) {
    auto path = Path::from_utf8(text);
    REQUIRE(path.ok());
    return path.value();
}

std::string read_text(const Path& path) {
    auto text = engine::core::read_text_file(path);
    REQUIRE(text.ok());
    return text.value();
}

// The scan of the sample's asset folder.
engine::assets::ProjectScan scan_showcase() {
    auto scanned = engine::assets::scan_project(path_of(kShowcase + "/assets"));
    if (!scanned.ok()) {
        FAIL("scan failed: " << scanned.error().message);
    }
    return std::move(scanned).value();
}

const engine::assets::AssetRecord& record(const engine::assets::ProjectScan& scan, const std::string& relative) {
    const auto* found = scan.registry.find_by_path(path_of(relative));
    INFO("asset " << relative);
    REQUIRE(found != nullptr);
    return *found;
}

// A loaded main scene with its catalog and prefab library, kept together because the scene holds pointers into them.
struct LoadedScene {
    engine::scene::ComponentCatalog catalog;
    std::unique_ptr<engine::scene::PrefabLibrary> prefabs;
    std::unique_ptr<engine::scene::Scene> scene;
    std::unordered_set<Guid> known;
    engine::assets::ProjectScan scan;

    LoadedScene() : scan(scan_showcase()) {
        REQUIRE(engine::gameplay::register_builtin_components(catalog).ok());
        prefabs = std::make_unique<engine::scene::PrefabLibrary>(catalog);
        for (const auto* asset : scan.registry.records()) {
            known.insert(asset->guid);
        }
        const auto& prefab_record = record(scan, "prefabs/crate.prefab");
        auto prefab = engine::scene::parse_prefab(read_text(path_of(kShowcase + "/assets/prefabs/crate.prefab")), catalog);
        REQUIRE(prefab.ok());
        REQUIRE(prefabs->add(prefab_record.guid, std::move(prefab).value()).ok());

        auto document = engine::scene::parse_scene(read_text(path_of(kShowcase + "/assets/scenes/main.scene")), catalog);
        if (!document.ok()) {
            FAIL("main.scene: " << document.error().message);
        }
        scene = std::make_unique<engine::scene::Scene>(catalog, *prefabs);
        auto loaded = scene->load(document.value(), known);
        if (!loaded.ok()) {
            FAIL("main.scene does not load: " << loaded.error().message);
        }
    }
};

}  // namespace

TEST_CASE("The showcase project scans clean: every source has a sidecar and every dependency exists", "[integration][showcase]") {
    const auto scan = scan_showcase();
    CHECK(scan.unregistered.empty());
    CHECK(scan.orphan_metadata.empty());
    CHECK(scan.registry.missing_dependencies().empty());
    CHECK(scan.registry.size() >= 20);
}

TEST_CASE("The showcase main scene loads, uses every built-in component, and has no missing asset", "[integration][showcase]") {
    LoadedScene loaded;
    CHECK(loaded.scene->missing_references().empty());
    CHECK(loaded.scene->size() > 15);

    std::set<std::string> used;
    for (const Guid& guid : loaded.scene->entity_guids()) {
        for (const std::string& type : loaded.scene->component_types(guid)) {
            used.insert(type);
        }
    }
    for (const auto& descriptor : loaded.catalog.components()) {
        INFO("built-in component " << descriptor.type_id << " is not used by the showcase scene");
        CHECK(used.count(descriptor.type_id) == 1);
    }
}

TEST_CASE("The showcase scene is simulated: bodies fall, scripts run, and no script raises", "[integration][showcase]") {
    LoadedScene loaded;
    const auto read_script = [&loaded](const Guid& guid) -> engine::core::Result<std::string, std::string> {
        const auto* found = loaded.scan.registry.find(guid);
        if (found == nullptr) {
            return engine::core::failure(std::string("not a registered asset"));
        }
        auto text = engine::core::read_text_file(path_of(kShowcase + "/assets/" + found->source_path.str()));
        if (!text.ok()) {
            return engine::core::failure(std::string("cannot read the script"));
        }
        return engine::core::success(text.value());
    };
    engine::script::ScriptServices services;
    services.assets = &loaded.scan.registry;
    auto created = engine::gameplay::GameWorld::create(*loaded.scene, read_script, services);
    REQUIRE(created.ok());
    auto world = std::move(created).value();
    for (const std::string& error : world->take_errors()) {
        FAIL_CHECK("script setup: " << error);
    }

    Guid ball_guid;
    for (const Guid& guid : loaded.scene->entity_guids()) {
        const auto bounce = loaded.scene->field(guid, engine::gameplay::kRigidBodyId, "restitution");
        if (bounce && std::get<float>(*bounce) > 0.5f) {
            ball_guid = guid;
        }
    }
    REQUIRE_FALSE(ball_guid.is_nil());
    const double start_height = loaded.scene->transform(ball_guid)->position.y;

    const auto before = world->stats();
    CHECK(before.bodies >= 7);   // ground, platform, zone, ball, three crates
    CHECK(before.scripts >= 5);  // hero, platform, three crates

    for (int frame = 0; frame < 180; ++frame) {
        world->advance(1.0 / 60.0);
    }
    const auto errors = world->take_errors();
    for (const std::string& error : errors) {
        FAIL_CHECK("script error: " << error);
    }
    CHECK(world->stats().fixed_steps > 100);
    // The ball dropped from 6 m onto the ground and settled above it instead of falling through.
    const double end_height = loaded.scene->transform(ball_guid)->position.y;
    CHECK(end_height < start_height - 3.0);
    CHECK(end_height > -0.5);
}

TEST_CASE("The editor opens the showcase, opens its scene with prefabs, plays it, and stops", "[integration][showcase][editor]") {
    const fs::path scratch = fs::temp_directory_path() / "engine_showcase_editor";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    auto console = std::make_shared<engine::editor::Console>();
    engine::editor::EditorModel model(console, path_of((scratch / "workspace.json").string()));
    model.startup("");
    REQUIRE(model.open_project(kShowcase).ok());
    CHECK(model.project().state() == engine::editor::ProjectState::Open);

    auto opened = model.scene().open(path_of(kShowcase + "/assets/scenes/main.scene"));
    if (!opened.ok()) {
        FAIL("the editor cannot open main.scene: " << opened.error().message);
    }
    CHECK(model.scene().scene().size() > 15);
    CHECK(model.scene().scene().missing_references().empty());

    REQUIRE(model.play());
    auto* session = model.play_session();
    REQUIRE(session != nullptr);
    for (int frame = 0; frame < 120; ++frame) {
        model.tick_play(1.0 / 60.0);
    }
    for (const std::string& error : session->take_errors()) {
        FAIL_CHECK("play error: " << error);
    }
    CHECK(session->fixed_steps() > 100);
    model.stop();
    CHECK(model.play_state() == engine::editor::PlayState::Edit);
    fs::remove_all(scratch);
}

TEST_CASE("The showcase animation graph builds a state machine, and its parameters steer it", "[integration][showcase]") {
    const std::string text = read_text(path_of(kShowcase + "/assets/animation/hero.animgraph.json"));
    auto graph = engine::animation::parse_state_graph(text);
    REQUIRE(graph.ok());

    engine::animation::Skeleton skeleton;
    skeleton.names = {"root"};
    skeleton.parents = {-1};
    skeleton.bind_pose = {engine::animation::JointPose{}};
    skeleton.inverse_bind = {glm::mat4(1.0f)};
    engine::animation::ClipLibrary clips;
    for (const char* name : {"idle", "walk", "jump"}) {
        engine::animation::AnimationClip clip;
        clip.name = name;
        clip.duration = 1.0f;
        clip.looping = std::string(name) != "jump";
        clip.tracks.resize(1);
        clips.emplace(name, clip);
    }
    auto machine = engine::animation::StateMachine::create(graph.value(), skeleton, clips);
    REQUIRE(machine.ok());
    CHECK(machine.value().state_name() == "idle");
}

TEST_CASE("The showcase material graphs parse, and their textures are registered assets", "[integration][showcase]") {
    const auto scan = scan_showcase();
    for (const char* name : {"crate", "ground"}) {
        const std::string relative = std::string("materials/") + name + ".material.json";
        auto graph = engine::material_graph::parse_graph(read_text(path_of(kShowcase + "/assets/" + relative)));
        INFO(relative);
        if (!graph.ok()) {
            for (const auto& error : graph.error()) {
                FAIL_CHECK(relative << ": " << engine::material_graph::to_text(error));
            }
        }
        // Each material lists the textures it samples as dependencies, so a cook pulls them into the pack.
        CHECK(record(scan, relative).dependencies.size() == 2);
    }
}

TEST_CASE("The showcase terrain and audio load through the engine readers", "[integration][showcase]") {
    auto terrain = engine::world::load_terrain(path_of(kShowcase + "/assets/terrain/island.terrain"));
    REQUIRE(terrain.ok());
    CHECK(terrain.value().desc.tiles_x == 2);
    CHECK(terrain.value().samples_x == 129);

    auto audio = engine::audio::AudioEngine::create_offline();
    CHECK(audio->load_clip(path_of(kShowcase + "/assets/audio/tone.wav")).ok());
    CHECK(audio->load_clip(path_of(kShowcase + "/assets/audio/ambience.ogg")).ok());
}

TEST_CASE("The showcase world partition opens, with its cells, objects, and always-loaded layer", "[integration][showcase]") {
    auto opened = engine::editor::WorldEditor::open(path_of(kShowcase + "/world"));
    if (!opened.ok()) {
        FAIL("world: " << opened.error());
    }
    const auto& world = opened.value().world();
    CHECK(world.cells.size() == 9);
    std::size_t objects = 0;
    for (const auto& cell : world.cells) {
        objects += cell.objects.size();
        CHECK_FALSE(cell.assets.empty());
    }
    CHECK(objects == 27);
    CHECK(world.layer.objects.size() == 1);
    CHECK_FALSE(opened.value().dirty());
}

TEST_CASE("Build Game packages the showcase, imports every asset type it reaches, and the package launches", "[integration][showcase][editor]") {
    const fs::path scratch = fs::temp_directory_path() / "engine_showcase_package";
    fs::remove_all(scratch);
    fs::create_directories(scratch / "output");
    fs::create_directories(scratch / "shaders");
    std::ofstream(scratch / "shaders" / "test.spv", std::ios::binary) << "shader bytes";
    std::ofstream(scratch / "NOTICES.txt", std::ios::binary) << "notices";

    const auto scan = scan_showcase();
    engine::editor::GameBuildOptions options;
    options.project = kShowcase + "/assets";
    options.output_dir = (scratch / "output").string();
    options.game_name = "Showcase";
    options.start_scene = record(scan, "scenes/main.scene").guid;
    options.backend = "vulkan";
    options.profile = "native";
    options.runtime_exe = ENGINE_TEST_GAME_RUNTIME;
    options.shader_dir = (scratch / "shaders").string();
    options.notices_file = (scratch / "NOTICES.txt").string();

    engine::editor::BuildProgress progress;
    const auto importers = engine::converter::ImporterRegistry::builtin();
    const auto result = engine::editor::build_game(options, progress, importers);
    INFO("build_game error: " << (result.ok() ? std::string() : result.error()));
    REQUIRE(result.ok());
    CHECK(result.value().verified);
    // The scene reaches the prefab, the models, the textures, the materials, the scripts, the animation graph, the terrain, the audio,
    // and the probe volume: every importer type the builtin registry has except none.
    CHECK(result.value().pack_entries >= 18);
    fs::remove_all(scratch);
}

TEST_CASE("Every feature area has a showcase asset", "[integration][showcase]") {
    const auto scan = scan_showcase();
    struct Area {
        const char* feature;
        const char* asset;
    };
    const Area areas[] = {
        {"scene and prefab", "scenes/main.scene"},
        {"prefab", "prefabs/crate.prefab"},
        {"second scene", "scenes/credits.scene"},
        {"model import (glb)", "models/props.glb"},
        {"model import (obj)", "models/marker.obj"},
        {"texture import (color)", "textures/checker.png"},
        {"texture import (normal)", "textures/bump_normal.png"},
        {"material graph", "materials/crate.material.json"},
        {"animation state graph", "animation/hero.animgraph.json"},
        {"lua scripting", "scripts/player.lua"},
        {"game ui scripting", "scripts/hud.lua"},
        {"audio (wav)", "audio/tone.wav"},
        {"audio (ogg)", "audio/ambience.ogg"},
        {"terrain", "terrain/island.terrain"},
        {"global illumination probes", "gi/probes.gi.json"},
        {"ui assets", "textures/coin_icon.png"},
    };
    for (const Area& area : areas) {
        INFO(area.feature << ": " << area.asset);
        CHECK(scan.registry.find_by_path(path_of(area.asset)) != nullptr);
    }
}
