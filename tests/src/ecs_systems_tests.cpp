// Systems, access declarations, stage ordering, and frame sync points (US-014). CPU-only; these run on every operating system.

#include "engine/ecs/registry.hpp"
#include "engine/ecs/systems.hpp"

#include "engine/core/assert.hpp"
#include "engine/core/jobs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using engine::core::AssertHandler;
using engine::core::AssertionFailure;
using engine::core::JobSystem;
using engine::ecs::Access;
using engine::ecs::EntityHandle;
using engine::ecs::FrameReport;
using engine::ecs::FrameStatus;
using engine::ecs::Registry;
using engine::ecs::RejectReason;
using engine::ecs::Schedule;
using engine::ecs::ScheduleErrorKind;
using engine::ecs::SystemContext;
using engine::ecs::SystemDesc;
using engine::ecs::SystemFn;

namespace {

struct Position {
    Position() = default;
    Position(int a, int b) : x(a), y(b) {}
    int x = 0;
    int y = 0;
};

struct Velocity {
    Velocity() = default;
    Velocity(int a, int b) : dx(a), dy(b) {}
    int dx = 0;
    int dy = 0;
};

SystemDesc describe(std::string name, Access access, SystemFn run, std::vector<std::string> after = {}) {
    SystemDesc desc;
    desc.name = std::move(name);
    desc.access = std::move(access);
    desc.after = std::move(after);
    desc.run = std::move(run);
    return desc;
}

void noop(SystemContext&) {}

// Replaces the assert handler with one that throws, so a failed debug assertion inside a job becomes a Failed job.
class ScopedThrowingAsserts {
public:
    ScopedThrowingAsserts() : previous_(engine::core::set_assert_handler(&throw_on_assert)) {}
    ~ScopedThrowingAsserts() {
        engine::core::set_assert_handler(previous_);
    }

private:
    static void throw_on_assert(const AssertionFailure& failure) {
        throw std::runtime_error(std::string(failure.message));
    }
    AssertHandler previous_;
};

}  // namespace

TEST_CASE("Access treats a written type as readable and conflicts only when one side writes", "[unit]") {
    Access writer = Access().write<Position>();
    CHECK(writer.can_write<Position>());
    CHECK(writer.can_read<Position>());
    CHECK_FALSE(writer.can_write<Velocity>());

    // read<T> then write<T> keeps a single write declaration.
    Access both = Access().read<Position>().write<Position>();
    CHECK(both.can_write<Position>());

    CHECK_FALSE(Access().read<Position>().conflicts_with(Access().read<Position>()));
    CHECK(writer.conflicts_with(Access().read<Position>()));
    CHECK(Access().read<Position>().conflicts_with(writer));
    CHECK(writer.conflicts_with(Access().write<Position>()));
    CHECK_FALSE(writer.conflicts_with(Access().write<Velocity>()));
}

TEST_CASE("A running system sees the access it declared", "[unit]") {
    Registry registry;
    bool saw_write = false;
    bool saw_undeclared_write = true;

    Schedule schedule;
    REQUIRE(schedule.add(describe("declared", Access().write<Position>().read<Velocity>(),
                                  [&saw_write, &saw_undeclared_write](SystemContext& context) {
                                      saw_write = context.access().can_write<Position>();
                                      saw_undeclared_write = context.access().can_write<Velocity>();
                                  })).ok());
    REQUIRE(schedule.build().ok());

    JobSystem jobs(1);
    REQUIRE(schedule.run(registry, jobs).status == FrameStatus::Completed);
    CHECK(saw_write);
    CHECK_FALSE(saw_undeclared_write);
}

TEST_CASE("Writers of the same component are placed in different stages in registration order", "[unit]") {
    Schedule schedule;
    REQUIRE(schedule.add(describe("first", Access().write<Position>(), noop)).ok());
    REQUIRE(schedule.add(describe("second", Access().write<Position>(), noop)).ok());
    REQUIRE(schedule.build().ok());

    const auto stages = schedule.stages();
    REQUIRE(stages.size() == 2);
    CHECK(stages[0] == std::vector<std::string>{"first"});
    CHECK(stages[1] == std::vector<std::string>{"second"});
}

TEST_CASE("A writer and a reader of the same component are not placed in one stage", "[unit]") {
    Schedule schedule;
    REQUIRE(schedule.add(describe("reader", Access().read<Position>(), noop)).ok());
    REQUIRE(schedule.add(describe("writer", Access().write<Position>(), noop)).ok());
    REQUIRE(schedule.build().ok());

    CHECK(schedule.stages().size() == 2);
}

TEST_CASE("Readers and systems with disjoint access share a stage", "[unit]") {
    Schedule schedule;
    REQUIRE(schedule.add(describe("read_a", Access().read<Position>(), noop)).ok());
    REQUIRE(schedule.add(describe("read_b", Access().read<Position>(), noop)).ok());
    REQUIRE(schedule.add(describe("velocity", Access().write<Velocity>(), noop)).ok());
    REQUIRE(schedule.build().ok());

    const auto stages = schedule.stages();
    REQUIRE(stages.size() == 1);
    const std::vector<std::string> expected{"read_a", "read_b", "velocity"};
    CHECK(stages[0] == expected);
}

TEST_CASE("An explicit after places a dependent in a later stage even when it is registered first", "[unit]") {
    Schedule schedule;
    REQUIRE(schedule.add(describe("dependent", Access().write<Position>(), noop, {"prerequisite"})).ok());
    REQUIRE(schedule.add(describe("prerequisite", Access().write<Position>(), noop)).ok());
    REQUIRE(schedule.build().ok());

    const auto stages = schedule.stages();
    REQUIRE(stages.size() == 2);
    CHECK(stages[0] == std::vector<std::string>{"prerequisite"});
    CHECK(stages[1] == std::vector<std::string>{"dependent"});
}

TEST_CASE("Explicit dependencies between non-conflicting systems are honoured", "[unit]") {
    Schedule schedule;
    REQUIRE(schedule.add(describe("late", Access(), noop, {"early"})).ok());
    REQUIRE(schedule.add(describe("early", Access(), noop)).ok());
    REQUIRE(schedule.build().ok());

    const auto stages = schedule.stages();
    REQUIRE(stages.size() == 2);
    CHECK(stages[0] == std::vector<std::string>{"early"});
    CHECK(stages[1] == std::vector<std::string>{"late"});
}

TEST_CASE("Build rejects an unknown dependency, a self dependency, and a cycle", "[unit]") {
    {
        Schedule schedule;
        REQUIRE(schedule.add(describe("a", Access(), noop, {"missing"})).ok());
        const auto built = schedule.build();
        REQUIRE_FALSE(built.ok());
        CHECK(built.error().kind == ScheduleErrorKind::UnknownDependency);
        CHECK(built.error().system == "a");
    }
    {
        Schedule schedule;
        REQUIRE(schedule.add(describe("a", Access(), noop, {"a"})).ok());
        const auto built = schedule.build();
        REQUIRE_FALSE(built.ok());
        CHECK(built.error().kind == ScheduleErrorKind::SelfDependency);
    }
    {
        Schedule schedule;
        REQUIRE(schedule.add(describe("a", Access(), noop, {"b"})).ok());
        REQUIRE(schedule.add(describe("b", Access(), noop, {"a"})).ok());
        const auto built = schedule.build();
        REQUIRE_FALSE(built.ok());
        CHECK(built.error().kind == ScheduleErrorKind::Cycle);
    }
}

TEST_CASE("Add rejects empty names, duplicate names, and missing functions", "[unit]") {
    Schedule schedule;
    REQUIRE(schedule.add(describe("a", Access(), noop)).ok());

    const auto duplicate = schedule.add(describe("a", Access(), noop));
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error().kind == ScheduleErrorKind::DuplicateName);

    const auto empty = schedule.add(describe("", Access(), noop));
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().kind == ScheduleErrorKind::EmptyName);

    const auto missing = schedule.add(describe("b", Access(), SystemFn{}));
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().kind == ScheduleErrorKind::MissingFunction);
    CHECK(schedule.size() == 1);
}

TEST_CASE("Conflicting writers never run at the same time on a multi-worker pool", "[unit]") {
    Registry registry;
    for (int i = 0; i < 64; ++i) {
        registry.add<Position>(registry.create(), i, 0);
    }

    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    const auto writer = [&active, &peak](SystemContext& context) {
        const int now = ++active;
        int seen = peak.load();
        while (now > seen && !peak.compare_exchange_weak(seen, now)) {
        }
        for (const EntityHandle entity : context.query<Position>()) {
            context.write<Position>(entity)->x += 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        --active;
    };

    Schedule schedule;
    REQUIRE(schedule.add(describe("writer_a", Access().write<Position>(), writer)).ok());
    REQUIRE(schedule.add(describe("writer_b", Access().write<Position>(), writer)).ok());
    REQUIRE(schedule.build().ok());

    JobSystem jobs(4);
    constexpr int kFrames = 10;
    for (int frame = 0; frame < kFrames; ++frame) {
        const FrameReport report = schedule.run(registry, jobs);
        REQUIRE(report.status == FrameStatus::Completed);
    }

    CHECK(peak.load() == 1);
    // Both writers touched every entity once per frame, so no update was lost to a race.
    CHECK(registry.world().size() == 64);
    int sum = 0;
    for (const EntityHandle entity : registry.query<Position>()) {
        sum += registry.get<Position>(entity)->x;
    }
    CHECK(sum == (63 * 64 / 2) + 64 * 2 * kFrames);
}

TEST_CASE("Commands apply at the stage boundary, so the next stage sees them and the current one does not", "[unit]") {
    Registry registry;
    std::size_t seen_during_stage = 99;
    std::size_t seen_next_stage = 99;

    Schedule schedule;
    REQUIRE(schedule.add(describe(
                "creator", Access().read<Position>(),
                [&seen_during_stage](SystemContext& context) {
                    const auto pending = context.commands().create();
                    context.commands().add<Position>(pending, 7, 8);
                    seen_during_stage = context.query<Position>().size();
                })).ok());
    REQUIRE(schedule.add(describe(
                "observer", Access().read<Position>(),
                [&seen_next_stage](SystemContext& context) {
                    seen_next_stage = context.query<Position>().size();
                },
                {"creator"})).ok());
    REQUIRE(schedule.build().ok());

    JobSystem jobs(2);
    const FrameReport report = schedule.run(registry, jobs);

    CHECK(report.status == FrameStatus::Completed);
    CHECK(report.stages_applied == 2);
    CHECK(seen_during_stage == 0);
    CHECK(seen_next_stage == 1);
    REQUIRE(report.created.size() == 1);
    CHECK(report.created[0].system == "creator");
    CHECK(report.created[0].pending_id == 1);
    CHECK(registry.get<Position>(report.created[0].handle)->x == 7);
}

TEST_CASE("Two systems that destroy the same entity leave one destroy applied and one rejected", "[unit]") {
    Registry registry;
    const EntityHandle target = registry.create();
    registry.add<Position>(target, 1, 1);

    // Neither system declares access, so both run in the same stage. Their buffers apply in registration order.
    Schedule schedule;
    REQUIRE(schedule.add(describe("first_destroy", Access(), [target](SystemContext& context) {
                context.commands().destroy(target);
            })).ok());
    REQUIRE(schedule.add(describe("second_destroy", Access(), [target](SystemContext& context) {
                context.commands().destroy(target);
            })).ok());
    REQUIRE(schedule.build().ok());
    CHECK(schedule.stages().size() == 1);

    JobSystem jobs(4);
    const FrameReport report = schedule.run(registry, jobs);

    CHECK(report.status == FrameStatus::Completed);
    CHECK(report.commands_applied == 1);
    REQUIRE(report.rejected.size() == 1);
    CHECK(report.rejected[0].system == "second_destroy");
    CHECK(report.rejected[0].rejection.reason == RejectReason::EntityNotAlive);
    CHECK_FALSE(registry.world().alive(target));
}

TEST_CASE("A frame that is cancelled before it starts runs no system", "[unit]") {
    Registry registry;
    std::atomic<int> ran{0};

    Schedule schedule;
    REQUIRE(schedule.add(describe("never", Access(), [&ran](SystemContext&) { ++ran; })).ok());
    REQUIRE(schedule.build().ok());

    std::atomic<bool> cancel{true};
    JobSystem jobs(2);
    const FrameReport report = schedule.run(registry, jobs, &cancel);

    CHECK(report.status == FrameStatus::Cancelled);
    CHECK(report.stages_applied == 0);
    CHECK(ran.load() == 0);
}

TEST_CASE("Cancelling mid-frame discards the cancelled stage and keeps earlier sync points", "[unit]") {
    Registry registry;
    std::atomic<bool> cancel{false};
    std::atomic<int> late_ran{0};
    std::size_t visible_in_cancelled_stage = 0;

    Schedule schedule;
    // Stage 0: creates an entity that applies at the first sync point.
    REQUIRE(schedule.add(describe("early", Access(), [](SystemContext& context) {
                const auto pending = context.commands().create();
                context.commands().add<Position>(pending, 1, 1);
            })).ok());
    // Stage 1: sees the entity, asks for cancellation, and tries to destroy the entity. The stage must be discarded.
    REQUIRE(schedule.add(describe(
                "cancelling", Access().read<Position>(),
                [&cancel, &visible_in_cancelled_stage](SystemContext& context) {
                    const auto found = context.query<Position>();
                    visible_in_cancelled_stage = found.size();
                    for (const EntityHandle entity : found) {
                        context.commands().destroy(entity);
                    }
                    cancel.store(true);
                },
                {"early"})).ok());
    // Stage 2: never starts, because the frame ends at stage 1's sync point.
    REQUIRE(schedule.add(describe(
                "late", Access(),
                [&late_ran](SystemContext&) { ++late_ran; },
                {"cancelling"})).ok());
    REQUIRE(schedule.build().ok());

    JobSystem jobs(2);
    const FrameReport report = schedule.run(registry, jobs, &cancel);

    CHECK(report.status == FrameStatus::Cancelled);
    CHECK(report.stages_applied == 1);
    CHECK(visible_in_cancelled_stage == 1);
    CHECK(late_ran.load() == 0);
    CHECK(registry.world().size() == 1);
    CHECK(registry.query<Position>().size() == 1);
}

TEST_CASE("A system that throws fails the frame and discards its stage's commands", "[unit]") {
    Registry registry;
    const EntityHandle entity = registry.create();

    Schedule schedule;
    REQUIRE(schedule.add(describe("destroyer", Access(), [entity](SystemContext& context) {
                context.commands().destroy(entity);
            })).ok());
    REQUIRE(schedule.add(describe("thrower", Access(), [](SystemContext&) {
                throw std::runtime_error("boom");
            })).ok());
    REQUIRE(schedule.build().ok());

    JobSystem jobs(2);
    const FrameReport report = schedule.run(registry, jobs);

    CHECK(report.status == FrameStatus::Failed);
    CHECK(report.error == "thrower: boom");
    CHECK(report.stages_applied == 0);
    CHECK(report.commands_applied == 0);
    CHECK(registry.world().alive(entity));
}

TEST_CASE("A frame with no systems completes", "[unit]") {
    Registry registry;
    Schedule schedule;
    REQUIRE(schedule.build().ok());

    JobSystem jobs(1);
    const FrameReport report = schedule.run(registry, jobs);

    CHECK(report.status == FrameStatus::Completed);
    CHECK(report.stages_applied == 0);
}

TEST_CASE("Repeated frames keep each system's writes in dependency order", "[unit]") {
    Registry registry;
    constexpr int kEntities = 50;
    constexpr int kFrames = 20;
    for (int i = 0; i < kEntities; ++i) {
        registry.add<Position>(registry.create(), 0, 0);
    }

    // Each writer adds one per frame. The reader is ordered after both, so it sees the frame's total.
    long long observed_total = 0;
    Schedule schedule;
    REQUIRE(schedule.add(describe("first", Access().write<Position>(), [](SystemContext& context) {
                for (const EntityHandle entity : context.query<Position>()) {
                    context.write<Position>(entity)->x += 1;
                }
            })).ok());
    REQUIRE(schedule.add(describe("second", Access().write<Position>(), [](SystemContext& context) {
                for (const EntityHandle entity : context.query<Position>()) {
                    context.write<Position>(entity)->y += 1;
                }
            }, {"first"})).ok());
    REQUIRE(schedule.add(describe("reader", Access().read<Position>(), [&observed_total](SystemContext& context) {
                long long total = 0;
                for (const EntityHandle entity : context.query<Position>()) {
                    total += context.read<Position>(entity)->x + context.read<Position>(entity)->y;
                }
                observed_total = total;
            }, {"second"})).ok());
    REQUIRE(schedule.build().ok());

    JobSystem jobs(4);
    for (int frame = 0; frame < kFrames; ++frame) {
        REQUIRE(schedule.run(registry, jobs).status == FrameStatus::Completed);
    }

    CHECK(observed_total == 2LL * kEntities * kFrames);
}

#ifndef NDEBUG
TEST_CASE("A system that reads an undeclared component fails its frame in a debug build", "[unit]") {
    const ScopedThrowingAsserts guard;
    Registry registry;
    registry.add<Velocity>(registry.create(), 1, 1);

    Schedule schedule;
    REQUIRE(schedule.add(describe("undeclared", Access(), [](SystemContext& context) {
                (void)context.query<Velocity>();
            })).ok());
    REQUIRE(schedule.build().ok());

    JobSystem jobs(1);
    const FrameReport report = schedule.run(registry, jobs);

    CHECK(report.status == FrameStatus::Failed);
    CHECK(report.error.find("did not declare") != std::string::npos);
}
#endif
