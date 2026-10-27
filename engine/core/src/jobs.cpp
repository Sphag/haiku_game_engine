#include "engine/core/jobs.hpp"

#include "engine/core/assert.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace engine::core {

std::string_view to_string(JobState state) noexcept {
    switch (state) {
        case JobState::Pending:
            return "Pending";
        case JobState::Running:
            return "Running";
        case JobState::Succeeded:
            return "Succeeded";
        case JobState::Failed:
            return "Failed";
        case JobState::Cancelled:
            return "Cancelled";
    }
    return "Unknown";
}

namespace detail {

struct JobRecord {
    JobFn fn;
    JobState state = JobState::Pending;
    std::string error;
    std::size_t unresolved_dependencies = 0;
    std::vector<std::shared_ptr<JobRecord>> dependents;
    std::shared_ptr<std::atomic<bool>> cancel_requested = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<GroupRecord> group;
};

struct GroupRecord {
    bool cancelled = false;
    std::size_t live = 0;  // submitted jobs that are not terminal yet
    std::size_t prune_threshold = 64;
    std::vector<std::weak_ptr<JobRecord>> members;  // expired entries are pruned as the group grows
};

// All mutable state. One mutex guards every field, including the JobRecords it reaches.
struct Scheduler {
    std::mutex mutex;
    std::condition_variable work_ready;
    std::condition_variable state_changed;
    std::deque<std::shared_ptr<JobRecord>> ready;                       // runnable, FIFO
    std::unordered_map<JobRecord*, std::shared_ptr<JobRecord>> live;  // non-terminal jobs
    bool accepting = true;
    bool stopping = false;
};

}  // namespace detail

namespace {

// Set on worker threads. wait() uses it to refuse a wait that could deadlock the pool.
thread_local const detail::Scheduler* g_worker_of = nullptr;

bool is_worker_of(const detail::Scheduler& scheduler) noexcept {
    return g_worker_of == &scheduler;
}

// Moves a job to a terminal state, then settles its dependents. A dependent of a job that
// did not succeed is cancelled without running. Iterative, so long chains cannot overflow the stack.
// Caller holds scheduler.mutex.
void complete(detail::Scheduler& scheduler, std::shared_ptr<detail::JobRecord> first, JobState state, std::string error) {
    struct Step {
        std::shared_ptr<detail::JobRecord> job;
        JobState state;
        std::string error;
    };

    std::vector<Step> steps;
    steps.push_back({std::move(first), state, std::move(error)});
    while (!steps.empty()) {
        Step step = std::move(steps.back());
        steps.pop_back();

        detail::JobRecord& job = *step.job;
        job.state = step.state;
        job.error = std::move(step.error);
        // A terminal job never runs again. Release its captures now, not when the last handle goes away.
        job.fn = nullptr;
        scheduler.live.erase(&job);
        if (job.group) {
            --job.group->live;
        }

        for (const auto& dependent : job.dependents) {
            if (dependent->state != JobState::Pending) {
                continue;  // already cancelled, so it must not run
            }
            if (step.state == JobState::Succeeded) {
                if (--dependent->unresolved_dependencies == 0) {
                    scheduler.ready.push_back(dependent);
                    scheduler.work_ready.notify_one();
                }
            } else {
                steps.push_back({dependent, JobState::Cancelled, "a dependency did not succeed"});
            }
        }
        job.dependents.clear();
    }
    scheduler.state_changed.notify_all();
}

// Caller holds scheduler.mutex.
void request_cancel(detail::Scheduler& scheduler, const std::shared_ptr<detail::JobRecord>& job) {
    job->cancel_requested->store(true, std::memory_order_release);
    if (job->state == JobState::Pending) {
        complete(scheduler, job, JobState::Cancelled, "cancelled before it started");
    }
}

void run_worker(std::shared_ptr<detail::Scheduler> scheduler) {
    g_worker_of = scheduler.get();
    std::unique_lock lock(scheduler->mutex);
    for (;;) {
        scheduler->work_ready.wait(lock, [&] { return scheduler->stopping || !scheduler->ready.empty(); });
        if (scheduler->ready.empty()) {
            return;  // stopping, and nothing is left to run
        }

        std::shared_ptr<detail::JobRecord> job = std::move(scheduler->ready.front());
        scheduler->ready.pop_front();
        if (job->state != JobState::Pending) {
            continue;  // cancelled while it was queued
        }
        job->state = JobState::Running;
        JobFn fn = std::move(job->fn);
        const CancellationToken token(job->cancel_requested);
        lock.unlock();

        JobState result = JobState::Succeeded;
        std::string error;
        try {
            fn(token);
        } catch (const std::exception& e) {
            result = JobState::Failed;
            error = e.what();
        } catch (...) {
            result = JobState::Failed;
            error = "unknown exception";
        }
        fn = nullptr;  // destroy the task's captures outside the lock

        lock.lock();
        if (result == JobState::Succeeded && job->cancel_requested->load(std::memory_order_acquire)) {
            result = JobState::Cancelled;
            error = "cancelled while running";
        }
        complete(*scheduler, job, result, std::move(error));
    }
}

}  // namespace

JobHandle JobHandle::spawn(const std::shared_ptr<detail::Scheduler>& scheduler,
                           const std::shared_ptr<detail::GroupRecord>& group,
                           JobFn fn,
                           std::initializer_list<JobHandle> dependencies) {
    for (const JobHandle& dependency : dependencies) {
        ENGINE_ASSERT(dependency.scheduler_ == scheduler, "a dependency must belong to the same JobSystem");
    }

    auto job = std::make_shared<detail::JobRecord>();
    job->fn = std::move(fn);
    job->group = group;

    std::unique_lock lock(scheduler->mutex);
    if (!scheduler->accepting || (group && group->cancelled)) {
        job->state = JobState::Cancelled;
        job->error = scheduler->accepting ? "its group was cancelled" : "the JobSystem is shut down";
        return JobHandle(scheduler, std::move(job));
    }

    scheduler->live.emplace(job.get(), job);
    if (group) {
        if (group->members.size() >= group->prune_threshold) {
            group->members.erase(
                std::remove_if(group->members.begin(), group->members.end(),
                               [](const std::weak_ptr<detail::JobRecord>& member) { return member.expired(); }),
                group->members.end());
            group->prune_threshold = std::max<std::size_t>(64, group->members.size() * 2);
        }
        group->members.push_back(job);
        ++group->live;
    }

    bool doomed = false;
    for (const JobHandle& dependency : dependencies) {
        detail::JobRecord& upstream = *dependency.record_;
        if (upstream.state == JobState::Succeeded) {
            continue;
        }
        if (is_terminal(upstream.state)) {
            doomed = true;  // Failed or Cancelled: this job can never run
            continue;
        }
        upstream.dependents.push_back(job);
        ++job->unresolved_dependencies;
    }

    if (doomed) {
        complete(*scheduler, job, JobState::Cancelled, "a dependency did not succeed");
    } else if (job->unresolved_dependencies == 0) {
        scheduler->ready.push_back(job);
        scheduler->work_ready.notify_one();
    }
    return JobHandle(scheduler, std::move(job));
}

JobState JobHandle::state() const {
    ENGINE_ASSERT(valid(), "state() called on an empty JobHandle");
    std::unique_lock lock(scheduler_->mutex);
    return record_->state;
}

std::string JobHandle::error() const {
    ENGINE_ASSERT(valid(), "error() called on an empty JobHandle");
    std::unique_lock lock(scheduler_->mutex);
    return record_->error;
}

void JobHandle::wait() const {
    ENGINE_ASSERT(valid(), "wait() called on an empty JobHandle");
    ENGINE_ASSERT(!is_worker_of(*scheduler_), "a job must not wait on a job of its own JobSystem");
    std::unique_lock lock(scheduler_->mutex);
    scheduler_->state_changed.wait(lock, [&] { return is_terminal(record_->state); });
}

void JobHandle::cancel() const {
    ENGINE_ASSERT(valid(), "cancel() called on an empty JobHandle");
    std::unique_lock lock(scheduler_->mutex);
    request_cancel(*scheduler_, record_);
}

JobHandle TaskGroup::submit(JobFn fn, std::initializer_list<JobHandle> dependencies) {
    return JobHandle::spawn(scheduler_, group_, std::move(fn), dependencies);
}

void TaskGroup::wait() const {
    ENGINE_ASSERT(!is_worker_of(*scheduler_), "a job must not wait on a group of its own JobSystem");
    std::unique_lock lock(scheduler_->mutex);
    scheduler_->state_changed.wait(lock, [&] { return group_->live == 0; });
}

void TaskGroup::cancel() {
    std::unique_lock lock(scheduler_->mutex);
    group_->cancelled = true;

    std::vector<std::shared_ptr<detail::JobRecord>> victims;
    for (const auto& member : group_->members) {
        if (auto job = member.lock(); job && !is_terminal(job->state)) {
            victims.push_back(std::move(job));
        }
    }
    for (const auto& job : victims) {
        request_cancel(*scheduler_, job);
    }
}

JobSystem::JobSystem(std::size_t worker_count)
    : scheduler_(std::make_shared<detail::Scheduler>()),
      worker_count_(worker_count != 0 ? worker_count
                                      : std::max<std::size_t>(1, static_cast<std::size_t>(std::thread::hardware_concurrency()))) {
    workers_.reserve(worker_count_);
    try {
        for (std::size_t i = 0; i < worker_count_; ++i) {
            workers_.emplace_back(run_worker, scheduler_);
        }
    } catch (...) {
        shutdown(ShutdownPolicy::CancelPending);
        throw;
    }
}

JobSystem::~JobSystem() {
    shutdown(ShutdownPolicy::CancelPending);
}

JobHandle JobSystem::submit(JobFn fn, std::initializer_list<JobHandle> dependencies) {
    return JobHandle::spawn(scheduler_, nullptr, std::move(fn), dependencies);
}

TaskGroup JobSystem::create_group() {
    return TaskGroup(scheduler_, std::make_shared<detail::GroupRecord>());
}

void JobSystem::wait_idle() const {
    ENGINE_ASSERT(!is_worker_of(*scheduler_), "a job must not wait for its own JobSystem to become idle");
    std::unique_lock lock(scheduler_->mutex);
    scheduler_->state_changed.wait(lock, [&] { return scheduler_->live.empty(); });
}

void JobSystem::shutdown(ShutdownPolicy policy) {
    ENGINE_ASSERT(!is_worker_of(*scheduler_), "a job must not shut down its own JobSystem");
    // Serializes concurrent callers so only one of them joins the workers.
    std::lock_guard lifecycle(lifecycle_);
    if (workers_.empty()) {
        return;  // already shut down
    }

    std::unique_lock lock(scheduler_->mutex);
    scheduler_->accepting = false;
    if (policy == ShutdownPolicy::CancelPending) {
        // Copy first: cancelling removes entries from live.
        std::vector<std::shared_ptr<detail::JobRecord>> victims;
        victims.reserve(scheduler_->live.size());
        for (const auto& entry : scheduler_->live) {
            victims.push_back(entry.second);
        }
        for (const auto& job : victims) {
            request_cancel(*scheduler_, job);
        }
    }
    scheduler_->state_changed.wait(lock, [&] { return scheduler_->live.empty(); });
    scheduler_->stopping = true;
    lock.unlock();

    scheduler_->work_ready.notify_all();
    for (std::thread& worker : workers_) {
        worker.join();
    }
    workers_.clear();
}

bool JobSystem::accepting() const {
    std::unique_lock lock(scheduler_->mutex);
    return scheduler_->accepting;
}

}  // namespace engine::core
