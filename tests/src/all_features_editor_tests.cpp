// The combined feature scene, editor half (US-079), headless: on a copy of the pack fixture project the scene is edited (create, remove,
// move) and undone and redone, a material graph and an animation graph are edited and undone, a terrain stroke is undone and saved, and
// a Play session runs the animation graph with a Lua driver before Stop. Then Build Game packages the project and launches the result
// from a clean copy. No window, graphics device, or ImGui is used.

#include "editor_fixture.hpp"

#include "asset_converter/importer.hpp"

#include "engine/animation/clip.hpp"
#include "engine/animation/graph_edit.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/editor/distribution.hpp"
#include "engine/editor/editor_model.hpp"
#include "engine/editor/project.hpp"
#include "engine/editor/play_session.hpp"
#include "engine/editor/scene_editor.hpp"
#include "engine/editor/terrain_editor.hpp"
#include "engine/material_graph/edit.hpp"
#include "engine/material_graph/graph.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/script/script_runner.hpp"
#include "engine/scene/transform.hpp"
#include "engine/world/terrain.hpp"
#include "engine/world/terrain_edit.hpp"

#include <glm/glm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;
using engine::core::Guid;
using engine::core::Path;
using engine::editor::BuildProgress;
using engine::editor::Console;
using engine::editor::EditorModel;
using engine::editor::GameBuildOptions;
using engine::editor::PlaySession;
using engine::editor::PlayState;
using engine::editor::TerrainEditor;
using engine::physics::kFixedStep;
using engine::scene::Transform;
using engine::tests::editor_fixture::Scratch;

constexpr const char* kStartScene = "3898c084-2f9e-4e6f-af6c-4ae6e8920395";

// A chunk that sets the animator's speed on every fixed step, so the walk transition is taken on the first step.
constexpr const char* kDriverScript = "function fixed_update(dt) animation.set_float('hero', 'speed', 1.0) end\n";

}  // namespace

TEST_CASE("The combined scene edits scene, material, animation, and terrain, undoes and redoes them, plays, and packages",
          "[integration][editor]") {
    using namespace engine::animation;
    using engine::material_graph::GraphDocument;
    using engine::material_graph::NodeKind;

    Scratch scratch;
    const fs::path root(scratch.path.str());

    // An editor project (project.json and an assets folder) whose assets are the pack fixture's scenes and data. The build scans the
    // assets folder as its asset root, so the editor and the package read the same sources.
    auto created = engine::editor::create_project(scratch.path, "all_features");
    REQUIRE(created.ok());
    const fs::path project = fs::path(created.value().root.str());
    const fs::path assets = project / engine::editor::kProjectAssetsFolder;
    fs::copy(fs::path(ENGINE_TEST_PACK_PROJECT) / "scenes", assets / "scenes", fs::copy_options::recursive);
    fs::copy(fs::path(ENGINE_TEST_PACK_PROJECT) / "data", assets / "data", fs::copy_options::recursive);

    auto console = std::make_shared<Console>();
    EditorModel model{console, scratch.path.join("workspace.json").value()};
    REQUIRE(model.open_project(project.string()).ok());

    // Scene: two entities, a move, a removal, then undo and redo of the removal.
    auto& scene = model.scene();
    const std::size_t base = scene.scene().size();
    auto hero = scene.create_entity();
    REQUIRE(hero.ok());
    Transform placed;
    placed.position = glm::dvec3(4.0, 0.0, -2.0);
    REQUIRE(scene.set_transform(hero.value(), placed).ok());
    auto prop = scene.create_entity();
    REQUIRE(prop.ok());
    CHECK(scene.scene().size() == base + 2);
    CHECK(scene.dirty());

    REQUIRE(scene.remove(prop.value()).ok());
    CHECK(scene.scene().size() == base + 1);
    REQUIRE(scene.undo());
    CHECK(scene.scene().size() == base + 2);
    REQUIRE(scene.redo());
    CHECK(scene.scene().size() == base + 1);
    CHECK_FALSE(scene.scene().contains(prop.value()));

    const fs::path scene_file = root / "all_features.scene.json";
    REQUIRE(scene.save_as(Path::from_utf8(scene_file.string()).value()).ok());
    CHECK_FALSE(scene.dirty());

    // Material graph: add a constant, set its value, undo, and redo. Every step stays valid.
    GraphDocument material = GraphDocument::create("all_features_tint");
    auto constant = material.add_node(NodeKind::Constant, glm::vec2(0.0f, 0.0f));
    REQUIRE(constant.ok());
    REQUIRE(material.set_value(constant.value(), glm::vec4(0.2f, 0.4f, 0.8f, 1.0f)).ok());
    CHECK(material.valid());
    REQUIRE(material.can_undo());
    REQUIRE(material.undo());
    REQUIRE(material.redo());
    CHECK(material.valid());

    // Animation graph: idle and walk, one float parameter, a transition with a condition on it.
    Skeleton skeleton;
    skeleton.names = {"root"};
    skeleton.parents = {-1};
    skeleton.bind_pose = {JointPose{}};
    skeleton.inverse_bind = {glm::mat4(1.0f)};
    AnimationClip clip;
    clip.duration = 1.0f;
    clip.tracks.resize(1);
    ClipLibrary clips{{"idle", clip}, {"walk", clip}};
    clips["idle"].name = "idle";
    clips["walk"].name = "walk";

    auto created_graph = AnimationGraphDocument::create(skeleton, clips);
    REQUIRE(created_graph.ok());
    AnimationGraphDocument& animation = created_graph.value();
    REQUIRE(animation.add_state("idle", "idle").ok());
    REQUIRE(animation.add_state("walk", "walk").ok());
    REQUIRE(animation.set_default_state("idle").ok());
    REQUIRE(animation.add_parameter(ParamDecl{"speed", ParamType::Float, 0.0f, false}).ok());
    auto transition = animation.add_transition("idle", "walk");
    REQUIRE(transition.ok());
    Condition fast;
    fast.kind = ConditionKind::Float;
    fast.parameter = "speed";
    fast.op = CompareOp::Greater;
    fast.value = 0.5f;
    REQUIRE(animation.add_condition(transition.value(), fast).ok());
    REQUIRE(animation.set_transition_timing(transition.value(), 0.25f, 0).ok());
    REQUIRE(animation.undo());
    REQUIRE(animation.redo());

    // Terrain: one stamp in a stroke, undone and redone, then saved and opened again.
    engine::world::TerrainDesc desc;
    desc.tiles_x = 2;
    desc.tiles_z = 1;
    desc.tile_cells = 8;
    desc.cell_size = 1.0f;
    auto terrain = TerrainEditor::create(desc);
    REQUIRE(terrain.ok());
    TerrainEditor& editor = terrain.value();
    engine::world::Brush raise;
    raise.kind = engine::world::BrushKind::Raise;
    raise.radius = 1.0f;
    raise.strength = 1.0f;
    raise.falloff = 0.0f;
    editor.brush() = raise;
    REQUIRE(editor.begin_stroke().ok());
    REQUIRE(editor.stamp(glm::vec2(4.0f, 4.0f)).ok());
    auto changed = editor.end_stroke();
    REQUIRE(changed.ok());
    CHECK(changed.value());
    REQUIRE(editor.undo());
    REQUIRE(editor.redo());
    const fs::path terrain_file = root / "all_features.terrain";
    REQUIRE(editor.save(Path::from_utf8(terrain_file.string()).value()).ok());
    REQUIRE(TerrainEditor::open(Path::from_utf8(terrain_file.string()).value()).ok());

    // Play: the scene is locked, the animation graph runs on the play world, and Stop restores the edited scene.
    const std::size_t edited_size = scene.scene().size();
    REQUIRE(model.play());
    CHECK(model.play_state() == PlayState::Playing);
    CHECK_FALSE(model.scene_editable());
    PlaySession* session = model.play_session();
    REQUIRE(session != nullptr);
    REQUIRE(session->animators().add("hero", StateMachine::create(animation.graph(), animation.skeleton(), animation.clips()).value()));
    const auto handle = session->scene().handle(hero.value());
    REQUIRE(handle.has_value());
    REQUIRE(session->runner().attach(*handle, "driver.lua", kDriverScript).ok());
    model.tick_play(kFixedStep);
    StateMachine* walker = session->animators().find("hero");
    REQUIRE(walker != nullptr);
    CHECK(walker->state_name() == "walk");
    CHECK(session->take_errors().empty());
    model.stop();
    CHECK(model.play_state() == PlayState::Edit);
    CHECK(PlaySession::live_count() == 0);
    CHECK(scene.scene().size() == edited_size);
    CHECK_FALSE(scene.dirty());

    // Package: Build Game stages the project, launches the package from a clean copy, and publishes it.
    const fs::path shaders = root / "shaders";
    fs::create_directories(shaders);
    std::ofstream(shaders / "test.spv", std::ios::binary) << "shader bytes";
    std::ofstream(root / "NOTICES.txt", std::ios::binary) << "notices";
    fs::create_directories(root / "output");

    GameBuildOptions options;
    options.project = assets.string();
    options.output_dir = (root / "output").string();
    options.game_name = "AllFeatures";
    options.start_scene = Guid::parse(kStartScene).value();
    options.backend = "vulkan";
    options.profile = "native";
    options.runtime_exe = ENGINE_TEST_GAME_RUNTIME;
    options.shader_dir = shaders.string();
    options.notices_file = (root / "NOTICES.txt").string();

    BuildProgress progress;
    const auto importers = engine::converter::ImporterRegistry::builtin();
    auto report = engine::editor::build_game(options, progress, importers);
    INFO("build_game error: " << (report.ok() ? std::string() : report.error()));
    REQUIRE(report.ok());
    CHECK(report.value().verified);
    CHECK(report.value().pack_entries == 2);
    CHECK(fs::exists(root / "output" / "AllFeatures" / "game.json"));
    CHECK_FALSE(fs::exists(root / "output" / "AllFeatures.staging"));

    // Relaunch: start the published folder through its own launcher, not the verify copy, and check that it loads the start scene.
    const fs::path published = root / "output" / "AllFeatures";
#ifdef _WIN32
    const std::string launcher = "run_game.bat";
    const std::string command = "cd /d \"" + published.string() + "\" && \".\\" + launcher + "\" --headless > \"" + (root / "relaunch.log").string() +
                                "\" 2>&1";
#else
    const std::string launcher = "run_game.sh";
    const std::string command = "cd '" + published.string() + "' && sh './" + launcher + "' --headless > '" + (root / "relaunch.log").string() +
                                "' 2>&1";
#endif
    CHECK(fs::exists(published / launcher));
    const int status = std::system(command.c_str());
    auto relaunch_log = engine::core::read_text_file(Path::from_utf8((root / "relaunch.log").string()).value());
    const std::string text = relaunch_log.ok() ? relaunch_log.value() : std::string();
    CHECK(status == 0);
    CHECK(text.find("game_runtime: started scene " + std::string(kStartScene)) != std::string::npos);
}
