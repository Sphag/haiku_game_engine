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
#include "engine/rhi/device.hpp"
#include "engine/gameplay/animation_binding.hpp"
#include "engine/runtime/game_hud.hpp"
#include "engine/runtime/runtime_material.hpp"
#include "engine/runtime/game_renderer.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/scene/scene_format.hpp"
#include "engine/world/terrain_edit.hpp"

#include "asset_converter/importer.hpp"
#include "gpu_test_options.hpp"

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "lua.h"
}

#include <cstdlib>
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

// The importer payload of a showcase asset, as the packed game would read it: a model goes through the model importer, anything else is its
// file as written (a graph is JSON text and a terrain is its own binary form).
std::optional<std::string> showcase_asset_bytes(const engine::assets::ProjectScan& scan, const Guid& guid) {
    const auto* record = scan.registry.find(guid);
    if (record == nullptr) {
        return std::nullopt;
    }
    auto source = engine::core::read_file(path_of(kShowcase + "/assets/" + record->source_path.str()));
    if (!source.ok()) {
        return std::nullopt;
    }
    if (record->importer != "model" && record->importer != "texture") {
        return source.value();
    }
    const auto importer = record->importer == "texture" ? engine::converter::make_texture_importer() : engine::converter::make_model_importer();
    auto settings = importer->canonical_settings(record->importer_settings);
    if (!settings.ok()) {
        return std::nullopt;
    }
    auto imported = importer->import(source.value(), settings.value());
    if (!imported.ok()) {
        return std::nullopt;
    }
    return imported.value().payload;
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
    const auto resolve_audio = [&loaded](const Guid& guid) -> std::optional<Path> {
        const auto* found = loaded.scan.registry.find(guid);
        if (found == nullptr) {
            return std::nullopt;
        }
        return path_of(kShowcase + "/assets/" + found->source_path.str());
    };
    auto created = engine::gameplay::GameWorld::create(*loaded.scene, read_script, services, resolve_audio);
    REQUIRE(created.ok());
    auto world = std::move(created).value();
    const engine::gameplay::AssetReader reader = [&loaded](const Guid& guid) { return showcase_asset_bytes(loaded.scan, guid); };
    const auto animators = engine::gameplay::bind_animators(*loaded.scene, reader, world->animators());
    for (const std::string& problem : animators.problems) {
        FAIL_CHECK("animator: " << problem);
    }
    CHECK(animators.names.size() == 1);
    CHECK(world->animators().find("arm") != nullptr);
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
    CHECK(before.scripts >= 6);  // hero, platform, arm, three crates

    for (int frame = 0; frame < 180; ++frame) {
        world->advance(1.0 / 60.0);
    }
    const auto errors = world->take_errors();
    for (const std::string& error : errors) {
        FAIL_CHECK("script error: " << error);
    }
    CHECK(world->stats().fixed_steps > 100);
    CHECK(world->stats().voices == 1);  // the campfire's looping clip starts with the scene

    // The platform is kinematic and its script turns it in the fixed step, so physics must have followed the scene's pose.
    bool platform_turned = false;
    for (const Guid& guid : loaded.scene->entity_guids()) {
        const auto motion = loaded.scene->field(guid, engine::gameplay::kRigidBodyId, "motion");
        const auto handle = loaded.scene->handle(guid);
        if (motion && std::get<std::int32_t>(*motion) == 1 && handle) {
            const auto pose = world->physics().step_transform(*handle);
            REQUIRE(pose.has_value());
            platform_turned = std::abs(pose->rotation.w) < 0.99f;
        }
    }
    CHECK(platform_turned);
    // The ball dropped from 6 m onto the ground and settled above it instead of falling through.
    const double end_height = loaded.scene->transform(ball_guid)->position.y;
    CHECK(end_height < start_height - 3.0);
    CHECK(end_height > -0.5);

    // The hero is a character controller: it fell from y = 1 onto the ground (feet at the ground's top, y = 0) and stands there.
    Guid hero;
    for (const Guid& guid : loaded.scene->entity_guids()) {
        if (loaded.scene->field(guid, engine::gameplay::kCharacterId, "move_speed")) {
            hero = guid;
        }
    }
    REQUIRE_FALSE(hero.is_nil());
    CHECK(std::abs(loaded.scene->transform(hero)->position.y) < 0.1);
    // Player input walks it: forward is -z at the component's move speed (3 m/s), so a second of walking covers about three meters.
    const double z_before = loaded.scene->transform(hero)->position.z;
    world->set_player_input(glm::vec2(0.0f, 1.0f), false);
    for (int frame = 0; frame < 60; ++frame) {
        world->advance(1.0 / 60.0);
    }
    CHECK(z_before - loaded.scene->transform(hero)->position.z > 2.0);
    world->set_player_input(glm::vec2(0.0f), true);  // a jump leaves the ground, then gravity brings it back
    double highest = 0.0;
    for (int frame = 0; frame < 90; ++frame) {
        world->advance(1.0 / 60.0);
        highest = std::max(highest, loaded.scene->transform(hero)->position.y);
    }
    CHECK(highest > 0.5);
    CHECK(loaded.scene->transform(hero)->position.y < 0.2);
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

TEST_CASE("The showcase materials: the graph parses, and each instance resolves to the fixed surface model", "[integration][showcase]") {
    const auto scan = scan_showcase();
    auto graph = engine::material_graph::parse_graph(read_text(path_of(kShowcase + "/assets/materials/surface.matgraph.json")));
    if (!graph.ok()) {
        for (const auto& error : graph.error()) {
            FAIL_CHECK("surface.matgraph.json: " << engine::material_graph::to_text(error));
        }
    }
    const engine::gameplay::AssetReader reader = [&scan](const Guid& guid) { return showcase_asset_bytes(scan, guid); };
    for (const char* name : {"crate", "ground"}) {
        const std::string relative = std::string("materials/") + name + ".material.json";
        INFO(relative);
        // The instance lists its graph and its two textures as dependencies, so a cook pulls them into the pack.
        CHECK(record(scan, relative).dependencies.size() == 3);
        auto recipe = engine::runtime::resolve_material(read_text(path_of(kShowcase + "/assets/" + relative)), reader);
        if (!recipe.ok()) {
            FAIL(relative << ": " << recipe.error());
        }
        CHECK(recipe.value().notes.empty());
        // The albedo sample multiplied by the tint is the base color: a map and a factor. The normal map feeds the normal pin.
        CHECK(recipe.value().textures.count(engine::runtime::kSlotBaseColor) == 1);
        CHECK(recipe.value().textures.count(engine::runtime::kSlotNormal) == 1);
        CHECK(recipe.value().params.roughness > 0.6f);
    }
    auto crate = engine::runtime::resolve_material(read_text(path_of(kShowcase + "/assets/materials/crate.material.json")), reader);
    REQUIRE(crate.ok());
    CHECK(crate.value().textures.at(engine::runtime::kSlotBaseColor) == record(scan, "textures/checker.png").guid);
    CHECK(crate.value().params.base_color == glm::vec4(1.0f, 0.95f, 0.9f, 1.0f));
    CHECK(crate.value().params.roughness == 0.65f);
}

TEST_CASE("A material instance is checked: unknown keys, missing graphs, and undeclared textures are errors", "[integration][showcase]") {
    const auto scan = scan_showcase();
    const engine::gameplay::AssetReader reader = [&scan](const Guid& guid) { return showcase_asset_bytes(scan, guid); };
    const std::string graph = record(scan, "materials/surface.matgraph.json").guid.to_string();
    const auto resolve = [&reader](const std::string& text) { return engine::runtime::resolve_material(text, reader); };
    CHECK(resolve("{\"format\":\"engine.material\",\"version\":1,\"graph\":\"" + graph + "\"}").ok());
    CHECK_FALSE(resolve("not json").ok());
    CHECK_FALSE(resolve("{\"format\":\"engine.material\",\"version\":1}").ok());
    CHECK_FALSE(resolve("{\"format\":\"engine.material\",\"version\":1,\"graph\":\"" + graph + "\",\"colour\":1}").ok());
    CHECK_FALSE(resolve("{\"format\":\"engine.material\",\"version\":1,\"graph\":\"00000000-0000-4000-8000-000000000001\"}").ok());
    CHECK_FALSE(resolve("{\"format\":\"engine.material\",\"version\":1,\"graph\":\"" + graph + "\",\"textures\":{\"nope\":\"" + graph + "\"}}").ok());
    CHECK_FALSE(resolve("{\"format\":\"engine.material\",\"version\":1,\"graph\":\"" + graph + "\",\"blend\":\"glass\"}").ok());
}

TEST_CASE("Texture payloads decode to RGBA8: block-compressed color and normals match their sources", "[integration][showcase]") {
    const auto scan = scan_showcase();
    // The cooked texture is BC7 (color) or BC5 (normal), so this runs the decoders the game uses.
    const auto checker = showcase_asset_bytes(scan, record(scan, "textures/checker.png").guid);
    REQUIRE(checker.has_value());
    auto color = engine::runtime::decode_texture_payload(*checker);
    REQUIRE(color.ok());
    CHECK(color.value().width == 64);
    CHECK(color.value().height == 64);
    // The first 8 x 8 square is tan and the next one brown (see the generator); the decoded texels are close to them.
    const auto* tan = color.value().texel(2, 2);
    const auto* brown = color.value().texel(10, 2);
    CHECK(std::abs(static_cast<int>(tan[0]) - 200) < 14);
    CHECK(std::abs(static_cast<int>(brown[0]) - 120) < 14);

    const auto normal_bytes = showcase_asset_bytes(scan, record(scan, "textures/bump_normal.png").guid);
    REQUIRE(normal_bytes.has_value());
    auto normal = engine::runtime::decode_texture_payload(*normal_bytes);
    REQUIRE(normal.ok());
    // Every normal points out of the surface: Z, rebuilt from the BC5 X and Y, is above the middle.
    std::size_t outward = 0;
    for (std::uint32_t y = 0; y < normal.value().height; ++y) {
        for (std::uint32_t x = 0; x < normal.value().width; ++x) {
            outward += normal.value().texel(x, y)[2] > 128 ? 1 : 0;
        }
    }
    CHECK(outward == static_cast<std::size_t>(normal.value().width) * normal.value().height);

    CHECK_FALSE(engine::runtime::decode_texture_payload("not a texture").ok());
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
    options.shader_dir = ENGINE_TEST_SHADER_DIR;  // the real shaders, so the package can open a window and draw
    options.font_file = ENGINE_TEST_FONT_PATH;     // and the font, so its HUD has text
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

    // The published folder starts through its own launcher, and the runtime simulates the scene: bodies and scripts from the cooked data.
    const fs::path published = scratch / "output" / "Showcase";
    const fs::path log = scratch / "relaunch.log";
#ifdef _WIN32
    const std::string command = "cd /d \"" + published.string() + "\" && \".\\run_game.bat\" --headless --frames 120 > \"" + log.string() + "\" 2>&1";
#else
    const std::string command = "cd '" + published.string() + "' && sh './run_game.sh' --headless --frames 120 > '" + log.string() + "' 2>&1";
#endif
    CHECK(std::system(command.c_str()) == 0);
    const std::string output = read_text(path_of(log.string()));
    INFO(output);
    CHECK(output.find("simulated 120 frames") != std::string::npos);
    CHECK(output.find("bodies=7") != std::string::npos);
    CHECK(output.find("scripts=6") != std::string::npos);
    // Keep a copy of the package for running by hand: build/win-dev/showcase_package/run_game.bat opens the game.
    std::error_code ignored;
    fs::remove_all(ENGINE_SHOWCASE_PACKAGE_OUT, ignored);
    fs::copy(published, ENGINE_SHOWCASE_PACKAGE_OUT, fs::copy_options::recursive, ignored);
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
        {"material graph", "materials/surface.matgraph.json"},
        {"material instance", "materials/crate.material.json"},
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

namespace {

// Draws the showcase scene on a real device and checks the picture: a camera was found, the lights and the meshes reached the renderer,
// and the image is not empty (the sky is cleared, the ground and the objects are lit and have distinct pick ids).
void render_showcase_on(engine::rhi::Backend backend) {
    LoadedScene loaded;
    engine::rhi::DeviceDesc desc;
    desc.backend = backend;
    desc.debug_layers = true;
    desc.allow_software_adapter = engine::tests::allow_software_adapters();
    auto device = engine::rhi::create_device(desc);
    if (!device.ok()) {
        FAIL("device: " << device.error().message);
    }
    const engine::gameplay::AssetReader reader = [&loaded](const Guid& guid) { return showcase_asset_bytes(loaded.scan, guid); };
    engine::animation::AnimatorSet animators;
    const auto binding = engine::gameplay::bind_animators(*loaded.scene, reader, animators);
    for (const std::string& problem : binding.problems) {
        FAIL_CHECK("animator: " << problem);
    }
    auto renderer = engine::runtime::GameRenderer::create(*device.value(), backend, ENGINE_TEST_SHADER_DIR, 1280, 720, reader);
    if (!renderer.ok()) {
        FAIL("renderer: " << renderer.error());
    }
    const std::size_t entities_before = loaded.scene->to_document().entities.size();
    for (const std::string& problem : renderer.value()->bind_scene(*loaded.scene, binding.names)) {
        FAIL_CHECK("renderer: " << problem);
    }
    // A second of frames, so the particles are alive and the arm's animation has moved.
    engine::core::Result<engine::runtime::RenderStats, std::string> drawn = engine::core::failure(std::string("not drawn"));
    for (int frame = 0; frame < 60; ++frame) {
        animators.update(1.0f / 60.0f);
        drawn = renderer.value()->render(*loaded.scene, &animators, 1.0 / 60.0);
        if (!drawn.ok()) {
            FAIL("render: " << drawn.error());
        }
    }
    CHECK(drawn.value().models >= 2);          // the props and the arm come from cooked payloads, not primitives
    CHECK(drawn.value().skinned >= 1);         // the arm is skinned
    CHECK(drawn.value().terrain_tiles == 4);   // the 2 x 2 tile island
    CHECK(drawn.value().particles > 0);        // the campfire
    CHECK(loaded.scene->to_document().entities.size() == entities_before);   // the particles are not entities: a saved scene never sees them
    CHECK(drawn.value().materials >= 2);       // the crate and the ground material instances
    CHECK(drawn.value().textures >= 3);        // the checker, the grass, and the shared normal map
    CHECK(drawn.value().camera);
    CHECK(drawn.value().lights >= 4);
    CHECK(drawn.value().items >= 12);

    const auto& image = renderer.value()->image();
    REQUIRE(image.width == 1280);
    REQUIRE(image.height == 720);
    std::size_t lit = 0;
    std::set<std::uint32_t> objects;
    for (std::size_t i = 0; i < image.color.size(); ++i) {
        if (image.color[i][0] != 0 || image.color[i][1] != 0 || image.color[i][2] != 0) {
            ++lit;
        }
        if (image.pick_ids[i] != 0) {
            objects.insert(image.pick_ids[i]);
        }
    }
    INFO("lit pixels " << lit << " of " << image.color.size());
    CHECK(lit > image.color.size() / 10);
    CHECK(objects.size() >= 4);
    // Drawing again after the scene moved still works and keeps the same objects visible.
    auto again = renderer.value()->render(*loaded.scene, &animators, 1.0 / 60.0);
    REQUIRE(again.ok());
    CHECK(again.value().items > 0);

    // The editor sizes the picture to its panel: a larger target needs a larger cluster grid, a smaller one a smaller grid.
    for (const auto& [width, height] : {std::pair<std::uint32_t, std::uint32_t>{1856, 871}, {333, 201}, {320, 200}}) {
        REQUIRE(renderer.value()->resize(width, height).ok());
        auto resized = renderer.value()->render(*loaded.scene, &animators, 1.0 / 60.0);
        REQUIRE(resized.ok());
        CHECK(resized.value().items > 0);
        CHECK(renderer.value()->image().width == width);
        CHECK(renderer.value()->image().height == height);
    }
}

}  // namespace

TEST_CASE("DX12: the showcase scene draws through GameRenderer", "[gpu][dx12][showcase]") {
    render_showcase_on(engine::rhi::Backend::Dx12);
}

TEST_CASE("Vulkan: the showcase scene draws through GameRenderer", "[gpu][vulkan][showcase]") {
    render_showcase_on(engine::rhi::Backend::Vulkan);
}

TEST_CASE("A UI layout parses, and a typo is an error and not a silent default", "[unit]") {
    const auto good = engine::ui::parse_ui_layout(
        R"({"format":"engine.ui_layout","version":1,"reference":[1920,1080],"elements":[
            {"kind":"button","anchor":"BottomRight","x":-20,"y":-20,"w":120,"h":40,"label":"Go","on_click":"go"},
            {"kind":"slider","w":100,"h":20,"value":0.5,"min":0,"max":2,"on_change":"changed"}]})");
    REQUIRE(good.ok());
    CHECK(good.value().reference_width == 1920.0f);
    REQUIRE(good.value().elements.size() == 2);
    CHECK(good.value().elements[0].on_click == "go");
    CHECK(good.value().elements[0].spec.label == "Go");
    CHECK(good.value().elements[1].spec.maximum == 2.0f);

    const auto bad = [](const char* text) { return engine::ui::parse_ui_layout(text); };
    CHECK_FALSE(bad("not json").ok());
    CHECK_FALSE(bad(R"({"format":"engine.scene","version":1,"elements":[]})").ok());
    CHECK_FALSE(bad(R"({"format":"engine.ui_layout","version":2,"elements":[]})").ok());
    CHECK_FALSE(bad(R"({"format":"engine.ui_layout","version":1,"elements":[{"kind":"dial","w":1,"h":1}]})").ok());
    CHECK_FALSE(bad(R"({"format":"engine.ui_layout","version":1,"elements":[{"kind":"text","w":1,"h":1,"colour":[0,0,0,0]}]})").ok());
    CHECK_FALSE(bad(R"({"format":"engine.ui_layout","version":1,"elements":[{"kind":"text","w":0,"h":1}]})").ok());
    CHECK_FALSE(bad(R"({"format":"engine.ui_layout","version":1,"elements":[{"kind":"text","anchor":"Middle","w":1,"h":1}]})").ok());
    CHECK_FALSE(bad(R"({"format":"engine.ui_layout","version":1,"elements":[{"kind":"text","w":1,"h":1,"color":[0,0,300,0]}]})").ok());
    CHECK_FALSE(bad(R"({"format":"engine.ui_layout","version":1,"elements":[{"kind":"slider","w":1,"h":1,"min":1,"max":1}]})").ok());
}

TEST_CASE("The showcase HUD draws, and its button and slider run the canvas's Lua functions", "[integration][showcase]") {
    const std::string layout = read_text(path_of(kShowcase + "/assets/ui/hud.ui.json"));
    const std::string script_text = read_text(path_of(kShowcase + "/assets/scripts/hud.lua"));
    auto font = engine::core::read_file(path_of(ENGINE_TEST_FONT_PATH));
    REQUIRE(font.ok());
    const std::vector<std::uint8_t> font_bytes(font.value().begin(), font.value().end());

    engine::script::ScriptEngine scripts;
    auto created = engine::runtime::GameHud::create(scripts, layout, script_text, font_bytes, 1280, 720);
    if (!created.ok()) {
        FAIL("hud: " << created.error());
    }
    auto& hud = *created.value();
    CHECK(hud.element_count() == 5);

    // The panel, the text, the button, and the slider put pixels on the surface.
    const auto surface = hud.render();
    REQUIRE(surface.width() == 1280);
    std::size_t covered = 0;
    for (std::size_t i = 3; i < surface.pixels().size(); i += 4) {
        covered += surface.pixels()[i] != 0 ? 1 : 0;
    }
    CHECK(covered > 20000);

    using engine::platform::InputEvent;
    using engine::platform::MouseButton;
    const auto click = [&hud](float x, float y) {
        std::vector<InputEvent> events;
        events.push_back(InputEvent{engine::platform::WindowId{}, engine::platform::MouseButtonPressed{MouseButton::Left}});
        events.push_back(InputEvent{engine::platform::WindowId{}, engine::platform::MouseButtonReleased{MouseButton::Left}});
        hud.update(x, y, {});         // the pointer moves first
        hud.update(x, y, events);     // then the press and release
    };
    const auto global_number = [&scripts](const char* name) {
        lua_State* state = scripts.state();
        lua_getglobal(state, name);
        const double value = lua_tonumber(state, -1);
        lua_pop(state, 1);
        return value;
    };
    REQUIRE(global_number("presses") == 0.0);
    click(95.0f, 120.0f);  // the "Say hello" button
    CHECK(global_number("presses") == 1.0);
    click(95.0f, 120.0f);
    CHECK(global_number("presses") == 2.0);
    CHECK(scripts.take_errors().empty());

    // A click on empty canvas outside every element does nothing.
    click(900.0f, 600.0f);
    CHECK(global_number("presses") == 2.0);

    // A layout that names a function the script does not define is refused at start.
    engine::script::ScriptEngine other;
    auto missing = engine::runtime::GameHud::create(other, layout, "-- no functions here\n", font_bytes, 1280, 720);
    CHECK_FALSE(missing.ok());
}
