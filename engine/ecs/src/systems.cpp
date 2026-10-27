#include "engine/ecs/systems.hpp"

#include <algorithm>
#include <utility>

namespace engine::ecs {

bool Access::contains(const std::vector<std::type_index>& types, std::type_index type) {
    return std::find(types.begin(), types.end(), type) != types.end();
}

void Access::add_read(std::type_index type) {
    if (!contains(writes_, type) && !contains(reads_, type)) {
        reads_.push_back(type);
    }
}

void Access::add_write(std::type_index type) {
    reads_.erase(std::remove(reads_.begin(), reads_.end(), type), reads_.end());
    if (!contains(writes_, type)) {
        writes_.push_back(type);
    }
}

bool Access::conflicts_with(const Access& other) const {
    for (const std::type_index type : writes_) {
        if (contains(other.reads_, type) || contains(other.writes_, type)) {
            return true;
        }
    }
    for (const std::type_index type : other.writes_) {
        if (contains(reads_, type)) {
            return true;
        }
    }
    return false;
}

core::Result<std::size_t, ScheduleError> Schedule::add(SystemDesc desc) {
    if (desc.name.empty()) {
        return core::failure(ScheduleError{ScheduleErrorKind::EmptyName, desc.name});
    }
    if (!desc.run) {
        return core::failure(ScheduleError{ScheduleErrorKind::MissingFunction, desc.name});
    }
    const bool duplicate = std::any_of(systems_.begin(), systems_.end(),
                                       [&desc](const Entry& entry) { return entry.desc.name == desc.name; });
    if (duplicate) {
        return core::failure(ScheduleError{ScheduleErrorKind::DuplicateName, desc.name});
    }

    built_ = false;
    systems_.push_back(Entry{std::move(desc), {}});
    return core::success(systems_.size() - 1);
}

core::Result<core::Unit, ScheduleError> Schedule::build() {
    const std::size_t count = systems_.size();

    for (std::size_t i = 0; i < count; ++i) {
        systems_[i].predecessors.clear();
        for (const std::string& name : systems_[i].desc.after) {
            if (name == systems_[i].desc.name) {
                return core::failure(ScheduleError{ScheduleErrorKind::SelfDependency, systems_[i].desc.name});
            }
            const auto found = std::find_if(systems_.begin(), systems_.end(),
                                            [&name](const Entry& entry) { return entry.desc.name == name; });
            if (found == systems_.end()) {
                return core::failure(ScheduleError{ScheduleErrorKind::UnknownDependency, systems_[i].desc.name});
            }
            systems_[i].predecessors.push_back(static_cast<std::size_t>(found - systems_.begin()));
        }
    }

    // Pack stages. A system is ready when every predecessor finished in an earlier stage. Ready systems are admitted in
    // registration order unless they conflict with a system already admitted to this stage. If no system is ready while
    // some remain, the explicit dependencies contain a cycle.
    stages_.clear();
    std::vector<bool> done(count, false);
    std::size_t remaining = count;
    while (remaining > 0) {
        std::vector<std::size_t> stage;
        for (std::size_t i = 0; i < count; ++i) {
            if (done[i]) {
                continue;
            }
            const bool ready = std::all_of(systems_[i].predecessors.begin(), systems_[i].predecessors.end(),
                                           [&done](std::size_t predecessor) { return done[predecessor]; });
            if (!ready) {
                continue;
            }
            const bool conflicts = std::any_of(stage.begin(), stage.end(), [this, i](std::size_t member) {
                return systems_[i].desc.access.conflicts_with(systems_[member].desc.access);
            });
            if (!conflicts) {
                stage.push_back(i);
            }
        }
        if (stage.empty()) {
            const auto stuck = static_cast<std::size_t>(std::find(done.begin(), done.end(), false) - done.begin());
            return core::failure(ScheduleError{ScheduleErrorKind::Cycle, systems_[stuck].desc.name});
        }
        for (const std::size_t member : stage) {
            done[member] = true;
        }
        remaining -= stage.size();
        stages_.push_back(std::move(stage));
    }

    buffers_.clear();
    buffers_.resize(count);
    built_ = true;
    return core::success();
}

FrameReport Schedule::run(Registry& registry, core::JobSystem& jobs, const std::atomic<bool>* cancel) {
    ENGINE_ASSERT(built_, "Schedule::build() must succeed before run()");

    FrameReport report;
    for (const std::vector<std::size_t>& stage : stages_) {
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            report.status = FrameStatus::Cancelled;
            return report;
        }

        core::TaskGroup group = jobs.create_group();
        std::vector<core::JobHandle> stage_jobs;
        stage_jobs.reserve(stage.size());
        for (const std::size_t index : stage) {
            Entry& entry = systems_[index];
            CommandBuffer& buffer = buffers_[index];
            stage_jobs.push_back(group.submit([&entry, &buffer, &registry, cancel](const core::CancellationToken&) {
                SystemContext context(registry, entry.desc.access, buffer, cancel);
                entry.desc.run(context);
            }));
        }
        group.wait();

        // Verify the stage before any of its commands apply: every job must have succeeded, and the frame must not have
        // been cancelled while the stage ran.
        FrameStatus status = FrameStatus::Completed;
        for (std::size_t k = 0; k < stage.size(); ++k) {
            const core::JobState state = stage_jobs[k].state();
            if (state == core::JobState::Failed) {
                status = FrameStatus::Failed;
                report.error = systems_[stage[k]].desc.name + ": " + stage_jobs[k].error();
                break;
            }
            if (state != core::JobState::Succeeded) {
                status = FrameStatus::Cancelled;
            }
        }
        if (status == FrameStatus::Completed && cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            status = FrameStatus::Cancelled;
        }
        if (status != FrameStatus::Completed) {
            for (const std::size_t index : stage) {
                buffers_[index].clear();
            }
            report.status = status;
            return report;
        }

        // Sync point. The stage is ascending, so buffers apply in registration order whatever order the workers finished in.
        for (const std::size_t index : stage) {
            const std::string& name = systems_[index].desc.name;
            ApplyReport applied = buffers_[index].apply(registry);
            report.commands_applied += applied.applied;
            for (const Rejection& rejection : applied.rejected) {
                report.rejected.push_back(RejectedCommand{name, rejection});
            }
            for (std::size_t k = 0; k < applied.created.size(); ++k) {
                report.created.push_back(CreatedEntity{name, static_cast<std::uint32_t>(k + 1), applied.created[k]});
            }
        }
        ++report.stages_applied;
    }
    return report;
}

std::vector<std::vector<std::string>> Schedule::stages() const {
    std::vector<std::vector<std::string>> names;
    names.reserve(stages_.size());
    for (const std::vector<std::size_t>& stage : stages_) {
        std::vector<std::string> row;
        row.reserve(stage.size());
        for (const std::size_t index : stage) {
            row.push_back(systems_[index].desc.name);
        }
        names.push_back(std::move(row));
    }
    return names;
}

}  // namespace engine::ecs
