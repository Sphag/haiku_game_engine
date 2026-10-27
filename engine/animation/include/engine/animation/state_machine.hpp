#pragma once

// Animation state machine runtime (US-069). A StateMachine binds a validated StateGraph to a skeleton and a clip library, and advances in
// fixed steps. Each update does, in order:
//   1. advance the active clip (and the outgoing clip while a blend runs) by dt times its state's speed;
//   2. record loop and finish events, and complete a blend whose time has elapsed;
//   3. if no blend runs, take the highest-priority transition whose conditions all hold, and start its blend;
//   4. sample the pose (a blend of the outgoing and entering clips while a blend runs) and the root-motion delta;
//   5. the triggers a taken transition tested are consumed.
// A blend runs for its transition's duration. No transition is taken while a blend runs, so a blend always completes first.
//
// Root motion. `root_delta()` is the change of the root's displacement during the update, in the frame of the entity: `translation` is in
// the root's own axes (playback.hpp) and `yaw` is the heading change in radians. The displacement is blended like the pose, so the deltas stay
// continuous through a blend, and the sum of the deltas is the entity's motion from the start of the machine.
//
// Events are queued until take_events() drains them. Loop and finish events describe the active state's clip, not the outgoing one.

#include "engine/animation/clip.hpp"
#include "engine/animation/playback.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_graph.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::animation {

// Clips by name. A machine copies the clips its states use, so the library may be discarded after create().
using ClipLibrary = std::map<std::string, AnimationClip, std::less<>>;

struct AnimationEvent {
    enum class Kind : std::uint8_t {
        Entered,   // a state became the active state (at creation, or when its transition started)
        Loop,      // the active clip wrapped; `cycle` is the cycle it reached from its start
        Finished,  // the active non-looping clip reached its end
    };
    Kind kind = Kind::Entered;
    std::uint32_t state = 0;  // index into the graph's states
    std::int64_t cycle = 0;
};

class StateMachine {
public:
    // Builds a machine. The graph must pass validate_state_graph; every state's clip must be in `clips` and valid for `skeleton`; and the
    // root joint, when the graph names one, must be a joint of the skeleton. The machine starts in the graph's default state at time zero.
    [[nodiscard]] static core::Result<StateMachine, std::string> create(const StateGraph& graph, const Skeleton& skeleton,
                                                                         const ClipLibrary& clips);

    // Advances by `dt` seconds (finite and non-negative) of fixed time. See the file comment for the order of one update.
    void update(float dt);

    // Parameter access by name. Each setter returns false when the name is not a parameter of the matching type.
    bool set_float(std::string_view name, float value);
    bool set_bool(std::string_view name, bool value);
    // Sets a trigger. It stays set until a transition that tests it is taken.
    bool set_trigger(std::string_view name);
    [[nodiscard]] std::optional<float> get_float(std::string_view name) const;
    [[nodiscard]] std::optional<bool> get_bool(std::string_view name) const;
    [[nodiscard]] std::optional<bool> get_trigger(std::string_view name) const;

    // The active state. While a blend runs it is the state being entered; the outgoing state is `source_state()`.
    [[nodiscard]] std::uint32_t state() const noexcept {
        return active_;
    }
    [[nodiscard]] std::string_view state_name() const noexcept;
    [[nodiscard]] bool blending() const noexcept {
        return blending_;
    }
    [[nodiscard]] std::uint32_t source_state() const noexcept {
        return source_;
    }
    // Progress of the running blend in [0, 1]: 0 at the start, 1 at the end. Reads 1 when no blend runs.
    [[nodiscard]] float blend_weight() const noexcept;
    // Unwrapped seconds of the active state's clip since it was entered (negative when it plays backward).
    [[nodiscard]] float state_time() const noexcept {
        return elapsed_;
    }

    // The pose and root delta produced by the last update (or by create, before the first update).
    [[nodiscard]] const std::vector<JointPose>& pose() const noexcept {
        return pose_;
    }
    [[nodiscard]] const RootDisplacement& root_delta() const noexcept {
        return root_delta_;
    }

    // The events queued since the last call, in the order they happened. The queue is emptied.
    std::vector<AnimationEvent> take_events();

    [[nodiscard]] const StateGraph& graph() const noexcept {
        return graph_;
    }

private:
    struct Param {
        ParamType type = ParamType::Float;
        float number = 0.0f;
        bool flag = false;  // the bool value, or whether a trigger is set
    };
    struct ResolvedCondition {
        ConditionKind kind = ConditionKind::Finished;
        std::size_t param = 0;
        CompareOp op = CompareOp::Greater;
        float value = 0.0f;
        bool expected = true;
    };

    StateMachine() = default;

    [[nodiscard]] std::optional<std::size_t> param_index(std::string_view name) const;
    [[nodiscard]] bool condition_holds(const ResolvedCondition& condition) const;
    [[nodiscard]] bool finished_now() const noexcept;
    [[nodiscard]] std::int64_t cycle_at(float elapsed) const noexcept;
    void advance(float dt);
    void take_transition(std::size_t transition);
    void evaluate();

    StateGraph graph_;
    Skeleton skeleton_;
    RootMotion root_motion_;
    std::vector<AnimationClip> clips_;                        // per state
    std::vector<std::vector<std::size_t>> outgoing_;          // per state: transition indices, highest priority first
    std::vector<std::vector<ResolvedCondition>> conditions_;  // per transition
    std::vector<std::uint32_t> targets_;                      // per transition: destination state index
    std::vector<Param> params_;                               // per parameter

    std::uint32_t active_ = 0;
    std::uint32_t source_ = 0;
    bool blending_ = false;
    float elapsed_ = 0.0f;         // active state's clip time
    float source_elapsed_ = 0.0f;  // outgoing state's clip time while blending
    float blend_time_ = 0.0f;
    float blend_duration_ = 0.0f;
    std::int64_t cycle_ = 0;       // last cycle of the active clip that was reported
    bool finished_ = false;        // the active clip has reached its end (reported)

    std::vector<JointPose> pose_;
    RootDisplacement root_total_;
    RootDisplacement root_delta_;
    std::vector<AnimationEvent> events_;
};

// Named machines that a host updates and scripts reach by name (US-069). The set owns its machines.
class AnimatorSet {
public:
    // Adds a machine under `name`. Returns false when the name is taken.
    bool add(std::string name, StateMachine machine);
    // The machine called `name`, or null. The pointer stays valid until the machine is removed.
    [[nodiscard]] StateMachine* find(std::string_view name) noexcept;
    bool remove(std::string_view name);
    // Advances every machine by dt.
    void update(float dt);
    [[nodiscard]] std::size_t size() const noexcept {
        return machines_.size();
    }

private:
    std::map<std::string, std::unique_ptr<StateMachine>, std::less<>> machines_;
};

}  // namespace engine::animation
