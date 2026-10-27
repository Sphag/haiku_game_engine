#include "engine/editor/play_session.hpp"

#include "engine/audio/audio_engine.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/script/script_runner.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

namespace engine::editor {

namespace {

std::atomic<std::size_t> g_live_sessions{0};

}  // namespace

PlaySession::PlaySession() {
    g_live_sessions.fetch_add(1, std::memory_order_relaxed);
}

PlaySession::~PlaySession() {
    // Stop what the session started before its systems go. The members are then released in reverse declaration order, so the
    // runner goes first, then the script engine (its destructor stops its voices in the audio engine), then audio and physics,
    // and the scene, whose registry the runner reads, goes last.
    if (runner_ != nullptr) {
        runner_->stop_all();
    }
    if (audio_ != nullptr) {
        audio_->stop_all();
    }
    if (physics_ != nullptr) {
        physics_->close();
    }
    g_live_sessions.fetch_sub(1, std::memory_order_relaxed);
}

core::Result<std::unique_ptr<PlaySession>, std::string> PlaySession::create(const scene::ComponentCatalog& catalog,
                                                                           const scene::PrefabLibrary& prefabs,
                                                                           const scene::SceneDocument& document,
                                                                           const std::unordered_set<core::Guid>& known_assets,
                                                                           gameplay::ScriptSource read_script,
                                                                           gameplay::AudioPath resolve_audio) {
    std::unique_ptr<PlaySession> session(new PlaySession());

    session->scene_ = std::make_unique<scene::Scene>(catalog, prefabs);
    auto loaded = session->scene_->load(document, known_assets);
    if (!loaded.ok()) {
        return core::failure(std::string("the scene does not load into a play world: ") + loaded.error().message);
    }

    auto physics = physics::PhysicsWorld::create(physics::PhysicsWorldDesc{});
    if (!physics.ok()) {
        return core::failure(std::string("the physics world could not start"));
    }
    session->physics_ = std::move(physics.value());

    session->audio_ = audio::AudioEngine::create_offline();

    session->scripts_ = std::make_unique<script::ScriptEngine>();
    script::ScriptServices services;
    services.registry = &session->scene_->mutable_registry();
    services.physics = session->physics_.get();
    services.audio = session->audio_.get();
    services.animators = &session->animators_;
    session->scripts_->bind(services);
    session->runner_ = std::make_unique<script::ScriptRunner>(*session->scripts_, session->scene_->registry());

    // The scene's rigid bodies and scripts start with it. A script that fails to load is reported and does not stop Play.
    auto bound = gameplay::bind_scene(*session->scene_, *session->physics_, *session->runner_, read_script, session->errors_,
                                          session->audio_.get(), resolve_audio);
    if (!bound.ok()) {
        return core::failure(bound.error());
    }
    session->bindings_ = std::move(bound.value());

    return core::success(std::move(session));
}

std::size_t PlaySession::live_count() noexcept {
    return g_live_sessions.load(std::memory_order_relaxed);
}

void PlaySession::set_paused(bool paused) {
    paused_ = paused;
    physics_->set_paused(paused);
    audio_->set_paused(paused);
    if (paused) {
        // Time that passes while paused is not owed to the clock when it resumes.
        accumulator_ = 0.0;
    }
}

void PlaySession::advance(double frame_seconds) {
    if (paused_ || !(frame_seconds > 0.0)) {
        return;
    }
    runner_->begin_frame();
    accumulator_ += frame_seconds;
    std::uint32_t steps = 0;
    while (accumulator_ >= physics::kFixedStep && steps < physics::kDefaultMaxCatchUpSteps) {
        accumulator_ -= physics::kFixedStep;
        run_fixed_step();
        ++steps;
    }
    if (accumulator_ >= physics::kFixedStep) {
        // The catch-up limit was reached. The rest of the frame's time is dropped, as the physics world drops it.
        accumulator_ = 0.0;
    }
    runner_->update(frame_seconds);
    gameplay::sync_scene(*scene_, *physics_, bindings_);
}

bool PlaySession::step() {
    if (!paused_) {
        return false;
    }
    runner_->begin_frame();
    // Physics ignores time while it is paused, so it runs for this one step only.
    physics_->set_paused(false);
    run_fixed_step();
    physics_->set_paused(true);
    runner_->update(physics::kFixedStep);
    gameplay::sync_scene(*scene_, *physics_, bindings_);
    return true;
}

void PlaySession::run_fixed_step() {
    const core::Status status = physics_->update(physics::kFixedStep);
    if (!status.ok()) {
        errors_.emplace_back("physics: the world is closed");
    }
    runner_->fixed_update(physics::kFixedStep);
    animators_.update(static_cast<float>(physics::kFixedStep));
    ++fixed_steps_;
}

std::vector<std::string> PlaySession::take_errors() {
    std::vector<std::string> out;
    out.swap(errors_);
    for (auto& error : runner_->take_errors()) {
        out.push_back(std::move(error.message));
    }
    for (auto& error : scripts_->take_errors()) {
        out.push_back(std::move(error.message));
    }
    return out;
}

}  // namespace engine::editor
