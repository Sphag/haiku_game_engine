#pragma once

#include "engine/ecs/command_buffer.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/registry.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/jobs.hpp"
#include "engine/core/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace engine::ecs {

// Component types a system reads and writes. A type that is written is also readable; a type is never listed twice.
class Access {
public:
    template <typename... Ts>
    Access& read() {
        (add_read(std::type_index(typeid(Ts))), ...);
        return *this;
    }
    template <typename... Ts>
    Access& write() {
        (add_write(std::type_index(typeid(Ts))), ...);
        return *this;
    }

    template <typename T>
    [[nodiscard]] bool can_read() const {
        return contains(reads_, std::type_index(typeid(T))) || contains(writes_, std::type_index(typeid(T)));
    }
    template <typename T>
    [[nodiscard]] bool can_write() const {
        return contains(writes_, std::type_index(typeid(T)));
    }

    // Two systems conflict when one writes a type that the other reads or writes. Read-only sharing does not conflict.
    [[nodiscard]] bool conflicts_with(const Access& other) const;

private:
    static bool contains(const std::vector<std::type_index>& types, std::type_index type);
    void add_read(std::type_index type);
    void add_write(std::type_index type);

    std::vector<std::type_index> reads_;
    std::vector<std::type_index> writes_;
};

// What a running system sees. Reads and writes are checked against the declared Access; a debug build asserts on an
// undeclared type. Structural changes go through commands(), so the registry does not change until the stage's sync point.
class SystemContext {
public:
    // Returns the entity's T, or nullptr when the entity is not alive or has no T.
    template <typename T>
    [[nodiscard]] const T* read(EntityHandle entity) const {
        ENGINE_DEBUG_ASSERT(access_->can_read<T>(), "system reads a component type it did not declare");
        return std::as_const(*registry_).get<T>(entity);
    }

    // Returns the entity's writable T, or nullptr when the entity is not alive or has no T.
    template <typename T>
    [[nodiscard]] T* write(EntityHandle entity) {
        ENGINE_DEBUG_ASSERT(access_->can_write<T>(), "system writes a component type it did not declare as written");
        return registry_->get<T>(entity);
    }

    // Live entities that hold every listed type. Membership is the contract; order is not.
    template <typename... Ts>
    [[nodiscard]] std::vector<EntityHandle> query() const {
        ENGINE_DEBUG_ASSERT((access_->can_read<Ts>() && ...), "system queries a component type it did not declare");
        return std::as_const(*registry_).query<Ts...>();
    }

    [[nodiscard]] bool alive(EntityHandle entity) const noexcept {
        return registry_->world().alive(entity);
    }

    // The access this system declared when it was added.
    [[nodiscard]] const Access& access() const noexcept {
        return *access_;
    }

    [[nodiscard]] CommandBuffer& commands() noexcept {
        return *buffer_;
    }

    // Long systems poll this to stop early. A frame that was cancelled discards this system's commands either way.
    [[nodiscard]] bool cancellation_requested() const noexcept {
        return cancel_ != nullptr && cancel_->load(std::memory_order_acquire);
    }

private:
    friend class Schedule;

    SystemContext(Registry& registry, const Access& access, CommandBuffer& buffer, const std::atomic<bool>* cancel) noexcept
        : registry_(&registry), access_(&access), buffer_(&buffer), cancel_(cancel) {}

    Registry* registry_;
    const Access* access_;
    CommandBuffer* buffer_;
    const std::atomic<bool>* cancel_;
};

using SystemFn = std::function<void(SystemContext&)>;

struct SystemDesc {
    std::string name;  // unique within a Schedule
    Access access;
    std::vector<std::string> after;  // systems that must finish first. Their commands apply at a sync point before this runs.
    SystemFn run;
};

enum class ScheduleErrorKind {
    EmptyName,
    DuplicateName,
    MissingFunction,
    UnknownDependency,  // `system` declared an `after` entry that is not registered
    SelfDependency,     // `system` lists itself in `after`
    Cycle               // `system` cannot be scheduled because explicit dependencies form a cycle
};

struct ScheduleError {
    ScheduleErrorKind kind = ScheduleErrorKind::EmptyName;
    std::string system;
};

enum class FrameStatus {
    Completed,
    Cancelled,  // the cancel flag was set, or a job of a stage was cancelled; the stage's commands were discarded
    Failed      // a system threw; `error` names it. The stage's commands were discarded
};

struct RejectedCommand {
    std::string system;
    Rejection rejection;
};

struct CreatedEntity {
    std::string system;
    std::uint32_t pending_id = 0;  // the PendingEntity id that the system received from commands().create()
    EntityHandle handle;
};

struct FrameReport {
    FrameStatus status = FrameStatus::Completed;
    std::size_t stages_applied = 0;  // sync points that were reached and whose commands were applied
    std::size_t commands_applied = 0;
    std::vector<RejectedCommand> rejected;
    std::vector<CreatedEntity> created;
    std::string error;  // for a Failed frame: the system name and the exception message
};

// Systems and their order. build() turns the declarations into stages: systems in one stage run at the same time as
// jobs, and each stage ends at a sync point where its command buffers are verified and applied.
//
// Ordering rules:
//   - `after` is an explicit dependency. A system starts only after its predecessors' stage has applied.
//   - Two systems that conflict (Access::conflicts_with) never share a stage. Among systems that are ready together, the
//     one registered first runs first. Conflicts with no explicit `after` therefore follow registration order.
//   - Systems that do not conflict and have no ordering between them share a stage.
//
// The schedule is not thread-safe. run() must not be called while another run() is in progress, and add() must not be
// called while a frame runs.
class Schedule {
public:
    // Registers a system and returns its registration index. Names are unique.
    [[nodiscard]] core::Result<std::size_t, ScheduleError> add(SystemDesc desc);

    // Resolves dependencies, rejects cycles, and packs the systems into stages. Call again after add().
    [[nodiscard]] core::Result<core::Unit, ScheduleError> build();

    // Runs one frame. Each stage's systems run as jobs on `jobs`. At the end of a stage, every job must have succeeded and
    // the cancel flag must be clear; then the stage's command buffers are applied in registration order. Otherwise the
    // stage's commands are discarded and the frame ends with the matching status. Earlier stages keep their applied changes.
    // `cancel` may be null. It is read by systems through SystemContext::cancellation_requested().
    [[nodiscard]] FrameReport run(Registry& registry, core::JobSystem& jobs, const std::atomic<bool>* cancel = nullptr);

    // Stage contents by system name, in run order. Valid after a successful build().
    [[nodiscard]] std::vector<std::vector<std::string>> stages() const;

    [[nodiscard]] std::size_t size() const noexcept {
        return systems_.size();
    }

private:
    struct Entry {
        SystemDesc desc;
        std::vector<std::size_t> predecessors;  // registration indices from `after`
    };

    std::vector<Entry> systems_;
    std::vector<std::vector<std::size_t>> stages_;  // registration indices, ascending within each stage
    std::vector<CommandBuffer> buffers_;            // one per system, reused every frame
    bool built_ = false;
};

}  // namespace engine::ecs
