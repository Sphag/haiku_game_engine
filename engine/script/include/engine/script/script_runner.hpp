#pragma once

// Script lifecycle (US-056). A ScriptRunner attaches a Lua chunk to an entity. Each attached instance runs in its own environment:
// the chunk's globals and the callbacks it defines are private to the instance, and the environment falls back to the shared
// globals for reads. The host drives the runner once per step, in this order:
//
//   attach(entity, chunk, source)  runs the chunk once. It defines the callbacks as globals and makes no callback call. A syntax
//                                  error or a top-level error fails the attach, and no instance or subscription remains.
//   begin_frame()                  1. tears down instances whose entity was destroyed, with no destroy callback;
//                                  2. for each Pending instance in attach order: create(entity), then start(entity).
//   fixed_update(dt)               once per fixed step: fixed_update(entity, dt) on each Running instance, in attach order.
//   update(dt)                     once per frame, after the fixed steps: update(entity, dt) on each Running instance.
//   reload(id, chunk, source)      see ReloadPolicy below. The new code's create and start run at the next begin_frame.
//   detach(id), stop_all()         destroy(entity) on each Running instance, then the instance's environment and its event
//                                  subscriptions are released. stop_all() visits instances in attach order.
//
// Every callback is optional. create, start, update, fixed_update, and destroy receive the entity first; update and fixed_update
// receive dt second. A Pending instance (attached but not yet started) gets no destroy, because it never got create. A callback
// runs only while its entity is alive; an instance whose entity died is torn down at the next begin_frame, without destroy.
//
// Errors are isolated per instance. A callback that raises marks its instance Faulted: no later callback runs for it, not even
// destroy, and it stays in the runner until detach, stop_all, or reload. The other instances keep running. Every error is queued
// with the callback's name and taken with take_errors(), which the host prints to the console.
//
// Reload compiles and runs the new chunk in a fresh environment before anything changes, so a syntax error leaves the running
// instance and its state intact. Then, for a Running instance:
//   PreserveExports  the old code's export_state() value (when it defines one) is passed to the new code's import_state(value).
//                    The old destroy(entity) runs before the switch. Only exported values carry over.
//   ResetState       nothing carries over; the new code starts from its own initial values.
// A Faulted instance reloads to Pending, so a script repaired by reload recovers without a restart.
//
// Teardown releases every Lua reference the runner holds. Stop must reach the runner before its registry or the ScriptEngine is
// destroyed; the destructor releases the references without calling Lua callbacks.
//
// The runner is single-threaded and runs on the thread that owns the ScriptEngine.

#include "engine/core/result.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/script/script_engine.hpp"
#include "engine/script/script_error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace engine::script {

using ScriptId = std::uint64_t;

enum class ReloadPolicy {
    PreserveExports,
    ResetState,
};

enum class InstanceState {
    Pending,  // attached or reloaded; create and start run at the next begin_frame
    Running,  // create and start have run; every callback runs
    Faulted,  // a callback raised; no callback runs
};

class ScriptRunner {
public:
    // The registry decides which entities are alive. The engine must outlive the runner.
    ScriptRunner(ScriptEngine& engine, const ecs::Registry& registry);
    ~ScriptRunner();

    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;
    ScriptRunner(ScriptRunner&&) = delete;
    ScriptRunner& operator=(ScriptRunner&&) = delete;

    // Runs the chunk in a new environment and returns the instance id. A dead entity or a failing chunk leaves nothing behind.
    [[nodiscard]] core::Result<ScriptId, ScriptError> attach(ecs::EntityHandle entity, std::string_view chunk_name,
                                                              std::string_view source,
                                                              ReloadPolicy policy = ReloadPolicy::PreserveExports);

    // Destroys a Running instance, then releases it. Returns false for an unknown id.
    bool detach(ScriptId id);

    // Replaces the instance's code (see the header). A failing chunk returns its error and leaves the instance unchanged.
    [[nodiscard]] core::Result<core::Unit, ScriptError> reload(ScriptId id, std::string_view chunk_name, std::string_view source);

    void begin_frame();
    void fixed_update(double dt);
    void update(double dt);

    // Play-to-Stop: destroys every Running instance and releases all of them.
    void stop_all();

    // Save state (US-059). capture_state returns the instance's plain data as a JSON object: the globals of its environment that are
    // booleans, numbers, strings, or tables of those. Functions, userdata, and threads are skipped, because the chunk defines them. It
    // fails for a cyclic table, a non-finite number, a non-UTF-8 string, or a table whose keys are neither strings nor 1..n.
    // restore_state sets those globals from text made by capture_state. It runs only on a Pending instance, so the values are in place
    // before create runs, and a bad text leaves the environment unchanged.
    [[nodiscard]] core::Result<std::string, ScriptError> capture_state(ScriptId id);
    [[nodiscard]] core::Result<core::Unit, ScriptError> restore_state(ScriptId id, std::string_view state);

    [[nodiscard]] std::optional<InstanceState> state(ScriptId id) const;
    [[nodiscard]] std::size_t instance_count() const noexcept {
        return instances_.size();
    }

    // Errors raised by callbacks, reloads, and event handlers since the last call, in order. The same queue as
    // ScriptEngine::take_errors(), so the host has one place to print from.
    [[nodiscard]] std::vector<ScriptError> take_errors();

private:
    struct Instance {
        ScriptId id = 0;
        ecs::EntityHandle entity;
        ReloadPolicy policy = ReloadPolicy::PreserveExports;
        InstanceState state = InstanceState::Pending;
        std::uint64_t owner = 0;  // tags the event subscriptions this code generation makes
        int env_ref = 0;          // Lua registry reference to the instance's environment table
    };

    [[nodiscard]] core::Result<int, ScriptError> load(std::string_view chunk_name, std::string_view source, std::uint64_t owner);
    bool call_entity(const Instance& instance, const char* name, const double* dt);
    [[nodiscard]] int export_state(const Instance& instance);
    [[nodiscard]] bool import_state(const Instance& instance, int value_ref);
    void release(Instance& instance);
    void sweep_dead();
    [[nodiscard]] Instance* find(ScriptId id);

    ScriptEngine& engine_;
    const ecs::Registry& registry_;
    std::vector<Instance> instances_;
    ScriptId next_id_ = 1;
    std::uint64_t next_owner_ = 1;
};

}  // namespace engine::script
