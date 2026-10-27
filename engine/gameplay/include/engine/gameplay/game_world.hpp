#pragma once

// GameWorld: the glue that turns a loaded scene's components into running systems. A rigid body component becomes a Jolt body, a script
// component becomes an attached Lua instance, and every frame the fixed steps run physics and scripts, the bodies write their poses back
// to the scene, and the scene updates its world matrices. The editor's Play button and the standalone runtime both use it, so a scene
// behaves the same in both.
//
// What it does not do: draw. Rendering reads the same components through the render module and needs a device; GameWorld runs on the CPU
// and needs none, so the sample project can be simulated in tests and in a headless runtime.
//
// Limits: a body's pose is written back to a root entity only (a child follows its parent through the scene graph, not through physics),
// and a kinematic body is placed where the scene puts it at start.

#include "engine/animation/state_machine.hpp"
#include "engine/audio/audio_engine.hpp"
#include "engine/core/guid.hpp"
#include "engine/core/result.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/scene.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/script/script_runner.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine::gameplay {

// Reads the text of a script asset by GUID. The editor reads the project folder; the runtime reads the packs.
using ScriptSource = std::function<core::Result<std::string, std::string>(const core::Guid&)>;

// What binding a scene to its systems produced: the dynamic bodies whose poses are written back, and the entities that carry a script.
// Maps an audio asset to a file the audio engine can decode. The editor resolves it inside the project; a host without files returns nothing,
// and its audio emitters are reported and stay silent.
using AudioPath = std::function<std::optional<core::Path>(const core::Guid&)>;

struct SceneBindings {
    std::vector<std::pair<core::Guid, ecs::EntityHandle>> kinematic_bodies;  // follow their entity: its pose becomes the body's target each frame
    std::size_t voices = 0;                                                  // audio emitters that started playing
    std::vector<std::pair<core::Guid, ecs::EntityHandle>> dynamic_bodies;  // scene GUID and body entity of each dynamic body
    std::vector<core::Guid> scripted;  // scene GUIDs of entities that carry an attached script
    std::size_t bodies = 0;
};

// Creates a physics body for every rigid body component and attaches every enabled script component. A script that cannot be read or
// does not compile is added to `errors` and the others still run; a body the physics world refuses fails the call. The same function serves
// GameWorld and the editor's Play session, so a scene behaves the same in both.
[[nodiscard]] core::Result<SceneBindings, std::string> bind_scene(scene::Scene& scene, physics::PhysicsWorld& physics, script::ScriptRunner& runner,
                                                                 const ScriptSource& read_script, std::vector<std::string>& errors,
                                                                 audio::AudioEngine* audio = nullptr, const AudioPath& resolve_audio = {});

// Writes each dynamic body's pose to its entity and re-applies the transform of every scripted entity, so the scene graph marks them
// dirty (scripts write transforms in place), then updates the scene's world matrices.
void sync_scene(scene::Scene& scene, physics::PhysicsWorld& physics, const SceneBindings& bindings);

struct GameWorldStats {
    std::size_t bodies = 0;
    std::size_t scripts = 0;           // attached instances
    std::size_t scripts_running = 0;
    std::size_t scripts_faulted = 0;
    std::size_t voices = 0;            // audio emitters playing from the start
    std::uint64_t fixed_steps = 0;
    double seconds = 0.0;              // simulated time
};

class GameWorld {
public:
    // Builds bodies and attaches scripts for every entity in `scene`. The scene must outlive the world. A script that cannot be read or
    // does not compile is reported through take_errors() and leaves the rest running; a body the physics world refuses fails the build.
    // `extra` supplies the services the host owns and the world does not: input actions, the asset registry. Everything else (the entity
    // registry, physics, audio) the world binds itself.
    [[nodiscard]] static core::Result<std::unique_ptr<GameWorld>, std::string> create(scene::Scene& scene, ScriptSource read_script,
                                                                                    const script::ScriptServices& extra = {},
                                                                                    AudioPath resolve_audio = {});
    ~GameWorld();

    GameWorld(const GameWorld&) = delete;
    GameWorld& operator=(const GameWorld&) = delete;

    // Advances by frame_seconds: fixed steps for physics and scripts, then the frame update.
    void advance(double frame_seconds);

    [[nodiscard]] GameWorldStats stats() const;
    // Errors raised since the last call: physics, script attach, and script callbacks.
    [[nodiscard]] std::vector<std::string> take_errors();

    [[nodiscard]] physics::PhysicsWorld& physics() noexcept {
        return *physics_;
    }
    [[nodiscard]] script::ScriptEngine& scripts() noexcept {
        return *scripts_;
    }
    [[nodiscard]] audio::AudioEngine& audio() noexcept {
        return *audio_;
    }
    // The named animation machines scripts reach through animation.set_float and friends. The host adds machines; advance() updates them.
    [[nodiscard]] animation::AnimatorSet& animators() noexcept {
        return animators_;
    }

private:
    GameWorld() = default;

    scene::Scene* scene_ = nullptr;
    std::unique_ptr<physics::PhysicsWorld> physics_;
    std::unique_ptr<audio::AudioEngine> audio_;
    animation::AnimatorSet animators_;
    std::unique_ptr<script::ScriptEngine> scripts_;
    std::unique_ptr<script::ScriptRunner> runner_;
    SceneBindings bindings_;
    std::vector<std::string> errors_;
    double accumulator_ = 0.0;
    GameWorldStats stats_;
};

}  // namespace engine::gameplay
