// The combined feature scene (US-079) on the CPU. One scenario uses the generated all_features workload (its counts and digests from the
// benchmark table), a Lua gameplay script that drives 16 audio emitters and an animation state machine, a UI button whose Lua callback runs
// on click, a save slot round trip that carries the script state, and a material graph evaluated on the CPU. The script records feature
// markers, and the test checks that every marker and output appears in order. No renderer, window, or GPU is used, so this runs on every
// operating system. GPU profiles, forced geometry paths, and the editor scenario are separate checks and are not covered here.

#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/audio/audio_engine.hpp"
#include "engine/benchmark/manifest.hpp"
#include "engine/benchmark/workload.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"
#include "engine/material_graph/evaluate.hpp"
#include "engine/material_graph/graph.hpp"
#include "engine/platform/input_router.hpp"
#include "engine/save/save.hpp"
#include "engine/scene/reflection.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/script/script_ui.hpp"
#include "engine/ui/ui.hpp"

#include "material_graph_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>

extern "C" {
#include "lua.h"
}

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

using engine::animation::AnimationClip;
using engine::animation::AnimatorSet;
using engine::animation::ClipLibrary;
using engine::animation::JointPose;
using engine::animation::Skeleton;
using engine::animation::StateMachine;
using engine::animation::parse_state_graph;
using engine::audio::AudioEngine;
using engine::core::Guid;
using engine::core::Path;
using engine::platform::InputEvent;
using engine::platform::WindowId;
using engine::save::SaveData;
using engine::save::ScriptSnapshot;
using engine::save::SaveSlots;
using engine::script::ScriptEngine;
using engine::script::ScriptServices;
using engine::ui::Anchor;
using engine::ui::ElementKind;
using engine::ui::ElementSpec;
using engine::ui::kNoElement;
using engine::ui::Ui;

namespace {

using Catch::Matchers::WithinAbs;

constexpr float kStep = 1.0f / 64.0f;
constexpr int kEmitters = 16;  // the all_features table's audio_emitters count

std::string audio_fixture(const char* name) {
    return std::string(ENGINE_TEST_FIXTURE_DIR) + "/audio/" + name;
}

engine::bench::Manifest repository_manifest() {
    const auto path = Path::from_utf8(ENGINE_TEST_BENCH_MANIFEST);
    REQUIRE(path.ok());
    const auto text = engine::core::read_text_file(path.value());
    REQUIRE(text.ok());
    const auto manifest = engine::bench::parse_manifest(text.value());
    REQUIRE(manifest.ok());
    return manifest.value();
}

std::map<std::string, engine::bench::Workload> expected_workloads() {
    const auto path = Path::from_utf8(ENGINE_TEST_WORKLOAD_EXPECTED);
    REQUIRE(path.ok());
    const auto text = engine::core::read_text_file(path.value());
    REQUIRE(text.ok());
    const auto expected = engine::bench::parse_workloads(text.value());
    REQUIRE(expected.ok());
    return expected.value();
}

// A throwaway directory for the save slots, removed when the test ends.
struct Scratch {
    Path path;

    Scratch() : path(make_path()) {}
    ~Scratch() {
        static_cast<void>(engine::core::remove_all(path));
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

private:
    static Path make_path() {
        auto base = Path::from_utf8(std::filesystem::temp_directory_path().string());
        REQUIRE(base.ok());
        auto dir = base.value().join("engine_all_features_" + Guid::generate().to_string());
        REQUIRE(dir.ok());
        REQUIRE(engine::core::create_directories(dir.value()).ok());
        return dir.value();
    }
};

// One state machine for the hero: idle is the default, and a positive speed walks. The clips have no keys, so the pose stays the bind pose.
constexpr const char* kHeroGraphJson = R"({
  "format": "engine.animation_graph", "version": 1,
  "parameters": [
    {"name": "speed", "type": "float", "default": 0}
  ],
  "default_state": "idle",
  "states": [
    {"name": "idle", "clip": "idle", "speed": 1},
    {"name": "walk", "clip": "walk", "speed": 1}
  ],
  "transitions": [
    {"from": "idle", "to": "walk", "duration": 0.25, "priority": 1,
     "conditions": [{"when": "float", "parameter": "speed", "op": ">", "value": 0.1}]}
  ]
})";

StateMachine make_hero_machine() {
    Skeleton skeleton;
    skeleton.names = {"root"};
    skeleton.parents = {-1};
    skeleton.bind_pose = {JointPose{}};
    skeleton.inverse_bind = {glm::mat4(1.0f)};

    AnimationClip clip;
    clip.duration = 1.0f;
    clip.looping = true;
    clip.tracks.resize(1);
    ClipLibrary clips;
    clip.name = "idle";
    clips.emplace("idle", clip);
    clip.name = "walk";
    clips.emplace("walk", clip);

    auto parsed = parse_state_graph(kHeroGraphJson);
    REQUIRE(parsed.ok());
    auto created = StateMachine::create(parsed.value(), skeleton, clips);
    REQUIRE(created.ok());
    return std::move(created).value();
}

// Reads a global string from the engine's Lua state.
std::string global_text(ScriptEngine& engine, const char* name) {
    lua_State* state = engine.state();
    lua_getglobal(state, name);
    const char* text = lua_tostring(state, -1);
    std::string value = text != nullptr ? text : "";
    lua_pop(state, 1);
    return value;
}

// Runs a chunk that must succeed, and leaves the Lua stack empty.
void run_ok(ScriptEngine& engine, const std::string& source) {
    const auto result = engine.run("all_features", source);
    const std::string detail = result.ok() ? std::string() : result.error().message;
    INFO(detail);
    REQUIRE(result.ok());
    CHECK(engine.stack_depth() == 0);
}

}  // namespace

TEST_CASE("The all_features workload matches the benchmark table and its expected digests", "[integration]") {
    const auto manifest = repository_manifest();
    REQUIRE(manifest.scenes.count("all_features") == 1);
    const auto generated = engine::bench::generate_workload("all_features", manifest.scenes.at("all_features"));
    REQUIRE(generated.ok());

    const auto expected = expected_workloads();
    REQUIRE(expected.count("all_features") == 1);
    const auto differences = engine::bench::compare_workloads(expected.at("all_features"), generated.value());
    for (const auto& line : differences) {
        INFO(line);
    }
    CHECK(differences.empty());
    CHECK(generated.value().counts.at("instances") == 100000);
    CHECK(generated.value().counts.at("particles") == 50000);
    CHECK(generated.value().counts.at("audio_emitters") == kEmitters);
    CHECK(generated.value().counts.at("characters") == 50);
}

TEST_CASE("The combined scene runs gameplay, audio, animation, UI, save, and a material graph, and records every marker", "[integration]") {
    // The audio engine, the animation set, and the UI are the services the gameplay script drives. The host steps the animators and the
    // audio engine as a frame would.
    auto audio = AudioEngine::create_offline();
    AnimatorSet animators;
    REQUIRE(animators.add("hero", make_hero_machine()));

    ScriptEngine script;
    ScriptServices services;
    services.audio = audio.get();
    services.animators = &animators;
    script.bind(services);

    // The gameplay script. `mark` is global, so later chunks add their markers too. The markers are appended in order, so a missing or
    // reordered feature fails the check below.
    const std::string gameplay =
        "markers = {}\n"
        "marker_text = ''\n"
        "function mark(name)\n"
        "  markers[#markers + 1] = name\n"
        "  marker_text = table.concat(markers, ',')\n"
        "end\n"
        "local tone = audio.load('" + audio_fixture("tone_mono_s16.wav") + "')\n"
        "mark('audio.load')\n"
        "audio.set_listener(0, 0, 0, 0, 0, -1, 0, 1, 0)\n"
        "for i = 1, " + std::to_string(kEmitters) + " do\n"
        "  audio.play_at(tone, i * 2, 0, 0, { min = 1, max = 20, loop = true })\n"
        "end\n"
        "mark('audio.emitters')\n"
        "animation.set_float('hero', 'speed', 1.0)\n"
        "mark('animation.parameter')\n"
        "pressed = 0\n"
        "function on_play() pressed = pressed + 1 mark('ui.click') end\n"
        "mark('ui.bound')\n";
    run_ok(script, gameplay);
    CHECK(audio->voice_count() == static_cast<std::size_t>(kEmitters));

    // The host steps the animation set. The script's speed change takes the transition at this update, as in a game.
    animators.update(kStep);
    REQUIRE(animators.find("hero") != nullptr);
    CHECK(animators.find("hero")->state_name() == "walk");
    CHECK(animators.find("hero")->blending());
    run_ok(script, "mark('animation.transition')");

    // The UI button runs the Lua callback on activation, through the real VM.
    Ui ui;
    ui.set_viewport(1280, 720);
    ui.set_active(true);
    ElementSpec button;
    button.kind = ElementKind::Button;
    button.placement = engine::ui::place(Anchor::TopLeft, 0.0f, 0.0f, 100.0f, 50.0f);
    lua_getglobal(script.state(), "on_play");
    button.on_click = engine::script::ui_action(script, -1);
    lua_pop(script.state(), 1);
    REQUIRE(ui.add(kNoElement, std::move(button)).ok());
    ui.set_pointer(10.0f, 10.0f);
    (void)ui.handle({InputEvent{WindowId{}, engine::platform::MouseButtonPressed{engine::platform::MouseButton::Left}}});
    (void)ui.handle({InputEvent{WindowId{}, engine::platform::MouseButtonReleased{engine::platform::MouseButton::Left}}});
    CHECK(script.take_errors().empty());
    CHECK(global_text(script, "marker_text") ==
          "audio.load,audio.emitters,animation.parameter,ui.bound,animation.transition,ui.click");
    run_ok(script, "assert(pressed == 1, 'the click should run the callback once')");

    // The UI output is a rasterized surface of the viewport size, so the HUD has pixels to composite.
    const auto surface = ui.render();
    CHECK(surface.width() == 1280);
    CHECK(surface.height() == 720);

    // Save: the gameplay state (the hero's speed) goes into a slot, and reads back with its script snapshot.
    Scratch scratch;
    const SaveSlots slots(scratch.path);
    SaveData data;
    data.save_id = Guid::generate();
    data.scripts.push_back(ScriptSnapshot{Guid::generate(), "hero", R"({"speed":1.0})"});
    auto written = slots.write("all_features", data);
    REQUIRE(written.ok());

    engine::scene::ComponentCatalog catalog;
    auto loaded = slots.read("all_features", catalog);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().save_id == written.value());
    REQUIRE(loaded.value().scripts.size() == 1);
    CHECK(loaded.value().scripts.front().state == R"({"speed":1.0})");

    // Cooked material graph: the sample graph evaluates on the CPU with the tint instance value. The GPU compile of the same graph is
    // covered by the material graph compile tests, and is not repeated here.
    auto graph = engine::material_graph::parse_graph(engine::material_graph::tests::sample_graph_json());
    REQUIRE(graph.ok());
    const engine::material_graph::ParameterValues tint{{"tint", {1.0f, 0.0f, 0.0f, 1.0f}}};
    auto evaluator = engine::material_graph::SurfaceEvaluator::create(graph.value(), tint);
    REQUIRE(evaluator.ok());
    const auto material = evaluator.value().evaluate({0.5f, 0.5f});
    CHECK_THAT(material.base_color.r, WithinAbs(1.0, 1e-5));
    CHECK_THAT(material.base_color.g, WithinAbs(0.0, 1e-5));
    CHECK_THAT(material.roughness, WithinAbs(0.25, 1e-5));
}
