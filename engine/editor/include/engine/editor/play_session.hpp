#pragma once

// Play mode (US-065): a runtime world built from the editor's scene document. The session owns its own scene copy, a physics world,
// a script engine with its runner, and an offline audio engine, and advances them on one fixed step. Stop destroys the session, so
// nothing the play changed reaches the editor's scene: the editor keeps the document it had before Play.
//
// Clock: a playing session runs whole fixed steps (physics::kFixedStep) for the frame time it is given, and drops time beyond the
// catch-up limit. Pause stops the clock and pauses physics and audio. Step runs exactly one fixed step while paused.
//
// The session has no ImGui, window, or GPU dependency. It creates no RHI objects, so Play adds nothing to the renderer's live counts.
// Entity-bound components (bodies, scripts, animation, particles) come from the game's component catalog; this build's editor catalog
// has none, so the session advances the systems but binds no entity-level content to them yet (docs/editor.md).

#include "engine/animation/state_machine.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"
#include "engine/scene/reflection.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_format.hpp"

#include "engine/gameplay/game_world.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine::audio {
class AudioEngine;
}  // namespace engine::audio

namespace engine::physics {
class PhysicsWorld;
}  // namespace engine::physics

namespace engine::script {
class ScriptEngine;
class ScriptRunner;
}  // namespace engine::script

namespace engine::editor {

// Edit: no session, the editor's scene is edited. Playing and Paused: a session exists and its clock runs or is held.
enum class PlayState : std::uint8_t {
    Edit,
    Playing,
    Paused,
};

class PlaySession {
public:
    // Builds the runtime world from the document. The catalog and the library are not owned and must outlive the session, as they
    // do for the editor's scene. Fails with the reason when the document does not load or a system cannot start.
    [[nodiscard]] static core::Result<std::unique_ptr<PlaySession>, std::string> create(const scene::ComponentCatalog& catalog,
                                                                                       const scene::PrefabLibrary& prefabs,
                                                                                       const scene::SceneDocument& document,
                                                                                       const std::unordered_set<core::Guid>& known_assets,
                                                                                       gameplay::ScriptSource read_script = {},
                                                                                       gameplay::AudioPath resolve_audio = {});

    // Sessions alive in the process. Stop must bring it back to zero, so tests can check that no session outlives its Stop.
    [[nodiscard]] static std::size_t live_count() noexcept;

    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    PlaySession(PlaySession&&) = delete;
    PlaySession& operator=(PlaySession&&) = delete;
    ~PlaySession();

    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }
    // Pausing holds the clock and pauses physics and audio. Resuming lets the clock run again from where it stopped.
    void set_paused(bool paused);

    // Runs the fixed steps the frame time covers, then one scripts update and one scene update. Does nothing while paused.
    void advance(double frame_seconds);
    // Runs one fixed step of every system. Only a paused session steps; returns false otherwise.
    [[nodiscard]] bool step();

    // Fixed steps run since the session began.
    [[nodiscard]] std::uint64_t fixed_steps() const noexcept {
        return fixed_steps_;
    }
    // Script and physics failures since the last call, as text.
    [[nodiscard]] std::vector<std::string> take_errors();

    [[nodiscard]] scene::Scene& scene() noexcept {
        return *scene_;
    }
    [[nodiscard]] physics::PhysicsWorld& physics() noexcept {
        return *physics_;
    }
    [[nodiscard]] script::ScriptEngine& scripts() noexcept {
        return *scripts_;
    }
    [[nodiscard]] script::ScriptRunner& runner() noexcept {
        return *runner_;
    }
    [[nodiscard]] audio::AudioEngine& audio() noexcept {
        return *audio_;
    }
    // The named animation machines the Lua `animation` module reaches (US-069). The animation graph window reads them for its
    // Play-mode parameter monitor. Machines are added by the host and advance with the fixed step, after the scripts.
    [[nodiscard]] animation::AnimatorSet& animators() noexcept {
        return animators_;
    }

private:
    PlaySession();
    // Runs one fixed step in every system. The caller has set up the clock (pause state and accumulator).
    void run_fixed_step();

    // Declared so that each member outlives the ones that use it: the scene (read by the runner), physics and audio (bound to the
    // script engine as services), the animators (bound the same way), the script engine, then the runner. Destruction runs the other way.
    std::unique_ptr<scene::Scene> scene_;
    std::unique_ptr<physics::PhysicsWorld> physics_;
    std::unique_ptr<audio::AudioEngine> audio_;
    animation::AnimatorSet animators_;
    std::unique_ptr<script::ScriptEngine> scripts_;
    std::unique_ptr<script::ScriptRunner> runner_;
    gameplay::SceneBindings bindings_;  // the bodies and scripts the scene's components produced
    std::vector<std::string> errors_;
    double accumulator_ = 0.0;
    std::uint64_t fixed_steps_ = 0;
    bool paused_ = false;
};

}  // namespace engine::editor
