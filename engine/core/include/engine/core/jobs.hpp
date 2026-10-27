#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace engine::core {

namespace detail {
struct GroupRecord;
struct JobRecord;
struct Scheduler;
}  // namespace detail

enum class JobState {
    Pending,    // waiting for dependencies or for a worker
    Running,
    Succeeded,
    Failed,     // the task threw; JobHandle::error() holds the exception message
    Cancelled,  // cancelled before it started, a dependency did not succeed, or cancelled while running
};

[[nodiscard]] std::string_view to_string(JobState state) noexcept;

[[nodiscard]] constexpr bool is_terminal(JobState state) noexcept {
    return state != JobState::Pending && state != JobState::Running;
}

// Seen by a running task. A default-constructed token is never cancelled.
class CancellationToken {
public:
    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<const std::atomic<bool>> flag) noexcept : flag_(std::move(flag)) {}

    [[nodiscard]] bool cancellation_requested() const noexcept {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<const std::atomic<bool>> flag_;
};

// A task receives a token so long-running work can stop early. Throwing marks the job Failed.
using JobFn = std::function<void(const CancellationToken&)>;

// Refers to one submitted job. A handle keeps the scheduler alive, so it stays usable after
// its JobSystem is destroyed; by then every job is terminal.
class JobHandle {
public:
    JobHandle() = default;

    [[nodiscard]] bool valid() const noexcept {
        return record_ != nullptr;
    }

    [[nodiscard]] JobState state() const;

    // Exception message for a Failed job, or the reason for a Cancelled job. Empty otherwise.
    [[nodiscard]] std::string error() const;

    // Blocks until the job is terminal. Asserts when called from a job of the same JobSystem,
    // because that job could be waiting for a worker it occupies.
    void wait() const;

    // A pending job is cancelled at once. A running job is asked to stop through its token,
    // and it ends Cancelled even if it returns normally.
    void cancel() const;

private:
    friend class JobSystem;
    friend class TaskGroup;

    JobHandle(std::shared_ptr<detail::Scheduler> scheduler, std::shared_ptr<detail::JobRecord> record) noexcept
        : scheduler_(std::move(scheduler)), record_(std::move(record)) {}

    [[nodiscard]] static JobHandle spawn(const std::shared_ptr<detail::Scheduler>& scheduler,
                                         const std::shared_ptr<detail::GroupRecord>& group,
                                         JobFn fn,
                                         std::initializer_list<JobHandle> dependencies);

    std::shared_ptr<detail::Scheduler> scheduler_;
    std::shared_ptr<detail::JobRecord> record_;
};

// Jobs submitted together. Wait and cancel apply to the whole group.
class TaskGroup {
public:
    // The job runs after every dependency succeeds. A dependency that failed or was cancelled cancels this job.
    [[nodiscard]] JobHandle submit(JobFn fn, std::initializer_list<JobHandle> dependencies = {});

    // Blocks until every job submitted to the group is terminal.
    void wait() const;

    // Cancels pending jobs and asks running jobs to stop. Later submissions to the group are cancelled.
    void cancel();

private:
    friend class JobSystem;

    TaskGroup(std::shared_ptr<detail::Scheduler> scheduler, std::shared_ptr<detail::GroupRecord> group) noexcept
        : scheduler_(std::move(scheduler)), group_(std::move(group)) {}

    std::shared_ptr<detail::Scheduler> scheduler_;
    std::shared_ptr<detail::GroupRecord> group_;
};

enum class ShutdownPolicy {
    Drain,          // run every job already submitted, then stop
    CancelPending,  // cancel jobs that have not started, ask running jobs to stop, then stop
};

// Worker pool with a dependency-aware queue. Shut it down before destroying any subsystem
// that a job uses: after shutdown returns, no job is running and none will start.
class JobSystem {
public:
    // worker_count 0 uses std::thread::hardware_concurrency(), with at least one worker.
    explicit JobSystem(std::size_t worker_count = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Once shutdown has begun, a submitted job comes back Cancelled and never runs.
    [[nodiscard]] JobHandle submit(JobFn fn, std::initializer_list<JobHandle> dependencies = {});
    [[nodiscard]] TaskGroup create_group();

    // Blocks until no submitted job is pending or running. Asserts when called from a worker.
    void wait_idle() const;

    // Stops accepting work, applies the policy, waits for running jobs, and joins the workers.
    // Calling it again does nothing.
    void shutdown(ShutdownPolicy policy);

    [[nodiscard]] bool accepting() const;
    [[nodiscard]] std::size_t worker_count() const noexcept {
        return worker_count_;
    }

private:
    std::shared_ptr<detail::Scheduler> scheduler_;
    std::size_t worker_count_;
    std::mutex lifecycle_;  // guards workers_ across shutdown calls
    std::vector<std::thread> workers_;
};

}  // namespace engine::core
