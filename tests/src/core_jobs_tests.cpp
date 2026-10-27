#include "engine/core/assert.hpp"
#include "engine/core/jobs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using engine::core::CancellationToken;
using engine::core::JobFn;
using engine::core::JobHandle;
using engine::core::JobState;
using engine::core::JobSystem;
using engine::core::ShutdownPolicy;
using engine::core::is_terminal;
using engine::core::to_string;

namespace {

// Submits fn after upstream. A default-constructed upstream means no dependency.
JobHandle chain(JobSystem& jobs, JobFn fn, const JobHandle& upstream) {
    if (!upstream.valid()) {
        return jobs.submit(std::move(fn));
    }
    return jobs.submit(std::move(fn), {upstream});
}

// Records the order in which jobs ran. Jobs append from worker threads.
class OrderLog {
public:
    void append(const std::string& entry) {
        const std::lock_guard lock(mutex_);
        entries_.push_back(entry);
    }

    [[nodiscard]] std::vector<std::string> entries() const {
        const std::lock_guard lock(mutex_);
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> entries_;
};

// Blocks a worker until the test opens it.
class Gate {
public:
    void open() {
        {
            const std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

// Spins until a flag is set. Used to know that a job has started.
void wait_for_flag(const std::atomic<bool>& flag) {
    while (!flag.load()) {
        std::this_thread::yield();
    }
}

// Installs a handler that throws, so a failed assertion can be caught inside a job.
void throwing_handler(const engine::core::AssertionFailure& failure) {
    throw std::runtime_error(std::string(failure.message));
}

}  // namespace

TEST_CASE("Job state names are stable", "[unit]") {
    CHECK(to_string(JobState::Pending) == "Pending");
    CHECK(to_string(JobState::Running) == "Running");
    CHECK(to_string(JobState::Succeeded) == "Succeeded");
    CHECK(to_string(JobState::Failed) == "Failed");
    CHECK(to_string(JobState::Cancelled) == "Cancelled");
}

TEST_CASE("A submitted job runs and wait returns Succeeded", "[unit]") {
    JobSystem jobs(2);
    CHECK(jobs.worker_count() == std::size_t{2});
    std::atomic<bool> ran{false};

    const JobHandle handle = jobs.submit([&](const CancellationToken&) { ran = true; });
    handle.wait();

    CHECK(ran.load());
    CHECK(handle.state() == JobState::Succeeded);
    CHECK(handle.error().empty());
}

TEST_CASE("A default JobSystem has at least one worker", "[unit]") {
    JobSystem jobs(0);

    CHECK(jobs.worker_count() >= 1);
}

TEST_CASE("A dependent job runs after its dependency in a chain", "[unit]") {
    JobSystem jobs(4);
    OrderLog log;

    const JobHandle first = chain(jobs, [&](const CancellationToken&) { log.append("first"); }, JobHandle{});
    const JobHandle second = chain(jobs, [&](const CancellationToken&) { log.append("second"); }, first);
    const JobHandle third = chain(jobs, [&](const CancellationToken&) { log.append("third"); }, second);
    third.wait();

    const std::vector<std::string> expected{"first", "second", "third"};
    CHECK(log.entries() == expected);
}

TEST_CASE("A job with two dependencies runs only after both finish", "[unit]") {
    JobSystem jobs(4);
    OrderLog log;

    const JobHandle slow = jobs.submit([&](const CancellationToken&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        log.append("slow");
    });
    const JobHandle fast = jobs.submit([&](const CancellationToken&) { log.append("fast"); });
    const JobHandle join = jobs.submit([&](const CancellationToken&) { log.append("join"); }, {slow, fast});
    join.wait();

    CHECK(join.state() == JobState::Succeeded);
    const auto entries = log.entries();
    REQUIRE(entries.size() == std::size_t{3});
    CHECK(entries.back() == "join");
}

TEST_CASE("A job whose dependency failed is cancelled without running", "[unit]") {
    JobSystem jobs(2);
    std::atomic<bool> dependent_ran{false};

    const JobHandle failing = jobs.submit([](const CancellationToken&) { throw std::runtime_error("boom"); });
    const JobHandle dependent = jobs.submit([&](const CancellationToken&) { dependent_ran = true; }, {failing});
    dependent.wait();

    CHECK(failing.state() == JobState::Failed);
    CHECK(failing.error() == "boom");
    CHECK(dependent.state() == JobState::Cancelled);
    CHECK(dependent.error() == "a dependency did not succeed");
    CHECK_FALSE(dependent_ran.load());
}

TEST_CASE("A job depending on a cancelled job is cancelled, and so is its dependent", "[unit]") {
    JobSystem jobs(1);
    Gate gate;

    const JobHandle blocker = jobs.submit([&](const CancellationToken&) { gate.wait(); });
    std::atomic<bool> victim_ran{false};
    const JobHandle victim = jobs.submit([&](const CancellationToken&) { victim_ran = true; });
    const JobHandle dependent = jobs.submit([](const CancellationToken&) {}, {victim});
    const JobHandle grandchild = jobs.submit([](const CancellationToken&) {}, {dependent});

    victim.cancel();
    gate.open();
    jobs.wait_idle();

    CHECK(victim.state() == JobState::Cancelled);
    CHECK(dependent.state() == JobState::Cancelled);
    CHECK(grandchild.state() == JobState::Cancelled);
    CHECK_FALSE(victim_ran.load());
    CHECK(blocker.state() == JobState::Succeeded);
}

TEST_CASE("A task failure does not stop the worker or unrelated jobs", "[unit]") {
    JobSystem jobs(1);
    std::atomic<int> survivors{0};

    const JobHandle thrown_error = jobs.submit([](const CancellationToken&) { throw std::runtime_error("first"); });
    const JobHandle non_standard = jobs.submit([](const CancellationToken&) { throw 7; });
    const JobHandle healthy = jobs.submit([&](const CancellationToken&) { survivors = 42; });
    jobs.wait_idle();

    CHECK(thrown_error.state() == JobState::Failed);
    CHECK(thrown_error.error() == "first");
    CHECK(non_standard.state() == JobState::Failed);
    CHECK(non_standard.error() == "unknown exception");
    CHECK(healthy.state() == JobState::Succeeded);
    CHECK(survivors.load() == 42);
}

TEST_CASE("A pending job cancelled before a worker takes it never runs", "[unit]") {
    JobSystem jobs(1);
    Gate gate;
    const JobHandle blocker = jobs.submit([&](const CancellationToken&) { gate.wait(); });
    std::atomic<bool> ran{false};
    const JobHandle victim = jobs.submit([&](const CancellationToken&) { ran = true; });

    victim.cancel();
    CHECK(victim.state() == JobState::Cancelled);
    gate.open();
    blocker.wait();
    victim.wait();

    CHECK_FALSE(ran.load());
    CHECK(victim.error() == "cancelled before it started");
}

TEST_CASE("A running job observes cancellation through its token", "[unit]") {
    JobSystem jobs(1);
    std::atomic<bool> started{false};
    std::atomic<bool> observed{false};

    const JobHandle handle = jobs.submit([&](const CancellationToken& token) {
        started = true;
        while (!token.cancellation_requested()) {
            std::this_thread::yield();
        }
        observed = true;
    });
    wait_for_flag(started);
    handle.cancel();
    handle.wait();

    CHECK(observed.load());
    CHECK(handle.state() == JobState::Cancelled);
    CHECK(handle.error() == "cancelled while running");
}

TEST_CASE("A default token is never cancelled", "[unit]") {
    const CancellationToken token;

    CHECK_FALSE(token.cancellation_requested());
}

TEST_CASE("A group wait returns after every job in the group completes", "[unit]") {
    JobSystem jobs(4);
    auto group = jobs.create_group();
    std::atomic<int> count{0};
    std::vector<JobHandle> handles;

    for (int i = 0; i < 50; ++i) {
        handles.push_back(group.submit([&](const CancellationToken&) { ++count; }));
    }
    group.wait();

    CHECK(count.load() == 50);
    for (const JobHandle& handle : handles) {
        CHECK(handle.state() == JobState::Succeeded);
    }
}

TEST_CASE("Group cancel cancels pending jobs and rejects later submissions", "[unit]") {
    JobSystem jobs(1);
    auto group = jobs.create_group();
    Gate gate;
    const JobHandle running = group.submit([&](const CancellationToken&) { gate.wait(); });
    std::atomic<bool> queued_ran{false};
    const JobHandle queued = group.submit([&](const CancellationToken&) { queued_ran = true; });

    group.cancel();
    gate.open();
    group.wait();
    std::atomic<bool> late_ran{false};
    const JobHandle late = group.submit([&](const CancellationToken&) { late_ran = true; });
    late.wait();

    CHECK(running.state() == JobState::Cancelled);
    CHECK(queued.state() == JobState::Cancelled);
    CHECK_FALSE(queued_ran.load());
    CHECK(late.state() == JobState::Cancelled);
    CHECK(late.error() == "its group was cancelled");
    CHECK_FALSE(late_ran.load());
}

TEST_CASE("Jobs in a group can depend on jobs outside it", "[unit]") {
    JobSystem jobs(2);
    auto group = jobs.create_group();
    const JobHandle outside = jobs.submit([](const CancellationToken&) {});

    const JobHandle inside = group.submit([](const CancellationToken&) {}, {outside});
    group.wait();

    CHECK(inside.state() == JobState::Succeeded);
}

TEST_CASE("Drain shutdown runs every submitted job before stopping", "[integration]") {
    JobSystem jobs(2);
    std::atomic<int> count{0};
    constexpr int kChain = 20;
    JobHandle previous;
    for (int i = 0; i < kChain; ++i) {
        previous = chain(jobs, [&](const CancellationToken&) { ++count; }, previous);
    }

    jobs.shutdown(ShutdownPolicy::Drain);

    CHECK(count.load() == kChain);
    CHECK(previous.state() == JobState::Succeeded);
    CHECK_FALSE(jobs.accepting());
}

TEST_CASE("CancelPending shutdown cancels queued jobs and stops running ones", "[integration]") {
    JobSystem jobs(1);
    std::atomic<bool> started{false};
    const JobHandle running = jobs.submit([&](const CancellationToken& token) {
        started = true;
        while (!token.cancellation_requested()) {
            std::this_thread::yield();
        }
    });
    wait_for_flag(started);
    std::atomic<bool> queued_ran{false};
    const JobHandle queued = jobs.submit([&](const CancellationToken&) { queued_ran = true; });

    jobs.shutdown(ShutdownPolicy::CancelPending);

    CHECK(running.state() == JobState::Cancelled);
    CHECK(queued.state() == JobState::Cancelled);
    CHECK_FALSE(queued_ran.load());
}

TEST_CASE("Submissions after shutdown are cancelled and never run", "[integration]") {
    JobSystem jobs(2);
    jobs.shutdown(ShutdownPolicy::Drain);
    std::atomic<bool> ran{false};

    const JobHandle late = jobs.submit([&](const CancellationToken&) { ran = true; });

    CHECK(late.state() == JobState::Cancelled);
    CHECK(late.error() == "the JobSystem is shut down");
    CHECK_FALSE(ran.load());
}

TEST_CASE("Shutdown is idempotent and handles outlive the JobSystem", "[integration]") {
    JobHandle survivor;
    {
        JobSystem jobs(2);
        survivor = jobs.submit([](const CancellationToken&) {});
        jobs.shutdown(ShutdownPolicy::Drain);
        jobs.shutdown(ShutdownPolicy::CancelPending);
    }

    survivor.wait();
    CHECK(survivor.state() == JobState::Succeeded);
}

TEST_CASE("Repeated startup and shutdown leaves no jobs behind", "[integration]") {
    constexpr int kCycles = 24;
    constexpr int kChain = 8;
    int total = 0;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        std::atomic<int> count{0};
        {
            JobSystem jobs(static_cast<std::size_t>(1 + cycle % 4));
            JobHandle previous;
            for (int i = 0; i < kChain; ++i) {
                previous = chain(jobs, [&](const CancellationToken&) { ++count; }, previous);
            }
            jobs.wait_idle();
            CHECK(count.load() == kChain);
            if (cycle % 2 == 0) {
                jobs.shutdown(ShutdownPolicy::Drain);
            }
            // Odd cycles rely on the destructor (CancelPending) to finish the shutdown.
        }
        total += count.load();
    }

    CHECK(total == kCycles * kChain);
}

TEST_CASE("A job cannot wait on a job of its own JobSystem", "[integration]") {
    const auto previous = engine::core::set_assert_handler(&throwing_handler);
    struct Restore {
        engine::core::AssertHandler handler;
        ~Restore() {
            engine::core::set_assert_handler(handler);
        }
    } restore{previous};

    JobSystem jobs(1);
    const JobHandle target = jobs.submit([](const CancellationToken&) {});
    const JobHandle waiter = jobs.submit([target](const CancellationToken&) { target.wait(); });
    waiter.wait();

    CHECK(waiter.state() == JobState::Failed);
    CHECK(waiter.error() == "a job must not wait on a job of its own JobSystem");
    CHECK(target.state() == JobState::Succeeded);
}

TEST_CASE("Cancelled and finished jobs release their captures at once", "[integration]") {
    auto sentinel = std::make_shared<int>(0);
    JobSystem jobs(1);
    std::atomic<bool> started{false};
    const JobHandle blocker = jobs.submit([&](const CancellationToken& token) {
        started = true;
        while (!token.cancellation_requested()) {
            std::this_thread::yield();
        }
    });
    wait_for_flag(started);

    const JobHandle queued = jobs.submit([sentinel](const CancellationToken&) { ++*sentinel; });
    CHECK(sentinel.use_count() == 2);
    queued.cancel();
    CHECK(queued.state() == JobState::Cancelled);
    CHECK(sentinel.use_count() == 1);

    // The only worker is busy with the blocker, so stop it before a job that must actually run.
    blocker.cancel();
    blocker.wait();
    CHECK(blocker.state() == JobState::Cancelled);

    const JobHandle finished = jobs.submit([sentinel](const CancellationToken&) { ++*sentinel; });
    finished.wait();
    CHECK(finished.state() == JobState::Succeeded);
    CHECK(sentinel.use_count() == 1);
    CHECK(*sentinel == 1);
}

TEST_CASE("Concurrent shutdown calls both return once the workers have stopped", "[integration]") {
    JobSystem jobs(2);
    std::vector<JobHandle> submitted;
    for (int i = 0; i < 16; ++i) {
        submitted.push_back(jobs.submit([](const CancellationToken&) {}));
    }

    std::thread other([&] { jobs.shutdown(ShutdownPolicy::Drain); });
    jobs.shutdown(ShutdownPolicy::CancelPending);
    other.join();

    CHECK_FALSE(jobs.accepting());
    for (const JobHandle& job : submitted) {
        CHECK(is_terminal(job.state()));
    }
}
