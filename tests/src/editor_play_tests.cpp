// Play mode (US-065): Play builds a runtime world apart from the edited scene, Stop discards it, Pause and Step hold and advance the
// clock that drives physics, scripts, and audio, and 20 Play/Stop cycles leave no session, script instance, or voice behind. No window,
// graphics device, or audio device is used.

#include "editor_fixture.hpp"

#include "engine/animation/clip.hpp"
#include "engine/animation/state_graph.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/audio/audio_clip.hpp"
#include "engine/audio/audio_engine.hpp"
#include "engine/audio/mixer.hpp"
#include "engine/editor/editor_model.hpp"
#include "engine/editor/play_session.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/script/script_runner.hpp"
#include "engine/scene/transform.hpp"

#include <glm/glm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using engine::audio::AudioClip;
using engine::audio::VoiceDesc;
using engine::core::LogLevel;
using engine::editor::Console;
using engine::editor::EditorModel;
using engine::editor::PlaySession;
using engine::editor::PlayState;
using engine::physics::kFixedStep;
using engine::scene::Transform;
using engine::script::ScriptRunner;
using engine::tests::editor_fixture::Scratch;

namespace {

// A model with one entity in the edited scene, and the scene saved so the dirty state is clean before Play.
struct PlayFixture {
    Scratch scratch;
    std::shared_ptr<Console> console = std::make_shared<Console>();
    EditorModel model{console, scratch.path.join("workspace.json").value()};
    engine::core::Guid entity;

    PlayFixture() {
        entity = model.scene().create_entity().value();
        // Save the baseline, so the scene is clean. A scene that was never saved is dirty against the empty document.
        REQUIRE(model.scene().save_as(scratch.path.join("play_scene.json").value()).ok());
        REQUIRE_FALSE(model.scene().dirty());
    }
};

// A chunk whose fixed_update counts the steps it receives in the global `ticks`.
constexpr const char* kCountingScript = "ticks = 0\nfunction fixed_update(dt) ticks = ticks + 1 end\n";

}  // namespace

TEST_CASE("Play builds a separate world, and Stop leaves the edited scene as it was", "[unit][editor]") {
    PlayFixture fixture;
    const std::size_t edited_size = fixture.model.scene().scene().size();
    CHECK(fixture.model.play_state() == PlayState::Edit);
    CHECK(fixture.model.scene_editable());

    REQUIRE(fixture.model.play());
    CHECK(fixture.model.play_state() == PlayState::Playing);
    CHECK_FALSE(fixture.model.scene_editable());
    PlaySession* session = fixture.model.play_session();
    REQUIRE(session != nullptr);
    CHECK(&session->scene() != &fixture.model.scene().scene());
    CHECK(session->scene().size() == edited_size);
    CHECK_FALSE(fixture.model.play());  // a second Play while one runs is refused

    // A runtime change lives only in the play world.
    REQUIRE(session->scene().add_entity(Transform{}).ok());
    CHECK(session->scene().size() == edited_size + 1);
    CHECK(fixture.model.scene().scene().size() == edited_size);

    fixture.model.stop();
    CHECK(fixture.model.play_state() == PlayState::Edit);
    CHECK(fixture.model.play_session() == nullptr);
    CHECK(fixture.model.scene().scene().size() == edited_size);
    CHECK(fixture.model.scene().scene().contains(fixture.entity));
    CHECK_FALSE(fixture.model.scene().dirty());
    CHECK(PlaySession::live_count() == 0);
}

TEST_CASE("Pause holds the clock, and Step runs exactly one fixed step while paused", "[unit][editor]") {
    PlayFixture fixture;
    REQUIRE(fixture.model.play());
    PlaySession* session = fixture.model.play_session();
    REQUIRE(session != nullptr);

    // Two steps and a remainder: 2.5 fixed steps of time.
    fixture.model.tick_play(kFixedStep * 2.5);
    CHECK(session->fixed_steps() == 2);

    fixture.model.toggle_pause();
    CHECK(fixture.model.play_state() == PlayState::Paused);
    CHECK(session->physics().paused());
    CHECK(session->audio().paused());

    // Frame time passes without a step while paused.
    fixture.model.tick_play(1.0);
    CHECK(session->fixed_steps() == 2);

    fixture.model.step();
    CHECK(session->fixed_steps() == 3);
    fixture.model.step();
    CHECK(session->fixed_steps() == 4);

    fixture.model.toggle_pause();
    CHECK(fixture.model.play_state() == PlayState::Playing);
    CHECK_FALSE(session->physics().paused());
    CHECK_FALSE(session->audio().paused());
    fixture.model.tick_play(kFixedStep);
    CHECK(session->fixed_steps() == 5);

    // Step does nothing while the clock runs.
    fixture.model.step();
    CHECK(session->fixed_steps() == 5);
    fixture.model.stop();
}

TEST_CASE("Scripts of the play world get the fixed steps and are destroyed by Stop", "[unit][editor]") {
    PlayFixture fixture;
    REQUIRE(fixture.model.play());
    PlaySession* session = fixture.model.play_session();
    REQUIRE(session != nullptr);

    const auto handle = session->scene().handle(fixture.entity);
    REQUIRE(handle.has_value());
    auto attached = session->runner().attach(*handle, "counter.lua", kCountingScript);
    REQUIRE(attached.ok());

    fixture.model.tick_play(kFixedStep * 3.0);
    CHECK(session->fixed_steps() == 3);
    const auto state = session->runner().capture_state(attached.value());
    REQUIRE(state.ok());
    CHECK(state.value().find("\"ticks\":3") != std::string::npos);

    fixture.model.stop();
    CHECK(PlaySession::live_count() == 0);
}

TEST_CASE("A voice started in the play world is released when the session stops", "[unit][editor]") {
    PlayFixture fixture;
    REQUIRE(fixture.model.play());
    PlaySession* session = fixture.model.play_session();
    REQUIRE(session != nullptr);

    AudioClip clip;
    clip.channels = 1;
    clip.sample_rate = 48000;
    clip.samples.assign(48000, 0.1f);
    auto clip_id = session->audio().add_clip(std::move(clip));
    REQUIRE(clip_id.ok());
    auto voice = session->audio().play(clip_id.value(), VoiceDesc{});
    REQUIRE(voice.ok());
    CHECK(session->audio().is_playing(voice.value()));
    CHECK(session->audio().voice_count() == 1);

    fixture.model.stop();
    CHECK(PlaySession::live_count() == 0);
}

TEST_CASE("Closing the project stops a play world", "[unit][editor]") {
    PlayFixture fixture;
    REQUIRE(fixture.model.play());
    fixture.model.close_project();
    CHECK(fixture.model.play_state() == PlayState::Edit);
    CHECK(PlaySession::live_count() == 0);
}

TEST_CASE("Twenty Play and Stop cycles leave no session, script instance, or voice behind", "[unit][editor]") {
    PlayFixture fixture;
    const std::size_t edited_size = fixture.model.scene().scene().size();

    AudioClip clip;
    clip.channels = 1;
    clip.sample_rate = 48000;
    clip.samples.assign(4800, 0.1f);

    for (int cycle = 0; cycle < 20; ++cycle) {
        INFO("cycle " << cycle);
        REQUIRE(fixture.model.play());
        PlaySession* session = fixture.model.play_session();
        REQUIRE(session != nullptr);
        CHECK(PlaySession::live_count() == 1);

        const auto handle = session->scene().handle(fixture.entity);
        REQUIRE(handle.has_value());
        REQUIRE(session->runner().attach(*handle, "counter.lua", kCountingScript).ok());
        REQUIRE(session->runner().instance_count() == 1);

        AudioClip copy = clip;
        auto clip_id = session->audio().add_clip(std::move(copy));
        REQUIRE(clip_id.ok());
        REQUIRE(session->audio().play(clip_id.value(), VoiceDesc{}).ok());

        fixture.model.tick_play(kFixedStep * 2.0);
        fixture.model.toggle_pause();
        fixture.model.step();
        fixture.model.toggle_pause();
        fixture.model.tick_play(kFixedStep);
        CHECK(session->fixed_steps() == 4);

        fixture.model.stop();
        CHECK(fixture.model.play_session() == nullptr);
        CHECK(PlaySession::live_count() == 0);
        CHECK(fixture.model.scene().scene().size() == edited_size);
        CHECK_FALSE(fixture.model.scene().dirty());
    }
    CHECK(fixture.model.play_state() == PlayState::Edit);
}

namespace {

// One joint, and two one-second clips with no keys, so the pose is the bind pose. The graph has one float parameter that moves idle to walk.
engine::animation::StateMachine play_machine() {
    using namespace engine::animation;
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

    StateGraph graph;
    graph.parameters = {ParamDecl{"speed", ParamType::Float, 0.0f, false}};
    graph.states = {StateDecl{"idle", "idle", 1.0f}, StateDecl{"walk", "walk", 1.0f}};
    graph.default_state = "idle";
    Condition fast;
    fast.kind = ConditionKind::Float;
    fast.parameter = "speed";
    fast.op = CompareOp::Greater;
    fast.value = 0.5f;
    graph.transitions = {TransitionDecl{"idle", "walk", 0.25f, 0, {fast}}};
    auto created = StateMachine::create(graph, skeleton, clips);
    REQUIRE(created.ok());
    return std::move(created).value();
}

}  // namespace

TEST_CASE("A Play session advances its animators on the fixed step, after the scripts that set their parameters",
          "[unit][editor][animation]") {
    PlayFixture fixture;
    REQUIRE(fixture.model.play());
    PlaySession* session = fixture.model.play_session();
    REQUIRE(session != nullptr);

    REQUIRE(session->animators().add("hero", play_machine()));
    const auto handle = session->scene().handle(fixture.entity);
    REQUIRE(handle.has_value());
    // The script sets the speed on every fixed step, before the animators step, so the transition is taken on the first step.
    REQUIRE(session->runner().attach(*handle, "driver.lua", "function fixed_update(dt) animation.set_float('hero', 'speed', 1.0) end\n").ok());

    fixture.model.tick_play(kFixedStep);

    engine::animation::StateMachine* hero = session->animators().find("hero");
    REQUIRE(hero != nullptr);
    CHECK(hero->get_float("speed") == std::optional<float>(1.0f));
    CHECK(hero->state_name() == "walk");
    CHECK(session->take_errors().empty());

    fixture.model.stop();
    CHECK(PlaySession::live_count() == 0);
}
