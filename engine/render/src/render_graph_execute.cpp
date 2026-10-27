// Render graph execution (US-032): transient allocation, recording, one submission, and timestamp readback.

#include "engine/render/render_graph.hpp"

#include "engine/core/clock.hpp"
#include "engine/core/result.hpp"
#include "engine/rhi/command_list.hpp"
#include "engine/rhi/device.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace engine::render {
namespace {

rhi::RhiStatus create_slot(rhi::Device& device, const PhysicalSlot& slot, PhysicalResource& out) {
    if (slot.imported) {
        out = slot.imported_handles;
        return core::success();
    }
    if (slot.kind == ResourceKind::Texture) {
        auto created = device.create_texture(slot.texture);
        if (!created.ok()) {
            return rhi::fail(created.error().code, created.error().message);
        }
        out.texture = created.value();
        return core::success();
    }
    auto created = device.create_buffer(slot.buffer);
    if (!created.ok()) {
        return rhi::fail(created.error().code, created.error().message);
    }
    out.buffer = created.value();
    return core::success();
}

// Owns the device objects that one execution creates. release() destroys them in the order the submission used them. The destructor
// does the same on a failure path and drops the errors, because the failure that caused the return is the one to report.
class ExecutionResources {
public:
    ExecutionResources(rhi::Device& device, const std::vector<PhysicalSlot>& slots)
        : device_(device), slots_(slots), physical_(slots.size()), created_(slots.size(), false) {}
    ~ExecutionResources() {
        (void)release();
    }
    ExecutionResources(const ExecutionResources&) = delete;
    ExecutionResources& operator=(const ExecutionResources&) = delete;

    [[nodiscard]] std::vector<PhysicalResource>& physical() noexcept {
        return physical_;
    }
    void mark_created(std::size_t slot) noexcept {
        created_[slot] = !slots_[slot].imported;
    }
    void set_pool(rhi::QueryPoolHandle pool) noexcept {
        pool_ = pool;
    }
    [[nodiscard]] rhi::QueryPoolHandle pool() const noexcept {
        return pool_;
    }
    void set_fence(rhi::FenceHandle fence) noexcept {
        fence_ = fence;
    }
    void set_list(std::unique_ptr<rhi::CommandList> list) noexcept {
        list_ = std::move(list);
    }
    [[nodiscard]] rhi::CommandList& list() noexcept {
        return *list_;
    }

    rhi::RhiStatus release() {
        rhi::RhiStatus first = core::success();
        const auto keep = [&first](rhi::RhiStatus status) {
            if (!status.ok() && first.ok()) {
                first = std::move(status);
            }
        };
        list_.reset();
        if (fence_.valid()) {
            keep(device_.destroy(fence_));
            fence_ = {};
        }
        if (pool_.valid()) {
            keep(device_.destroy(pool_));
            pool_ = {};
        }
        for (std::size_t s = 0; s < created_.size(); ++s) {
            if (!created_[s]) {
                continue;
            }
            keep(slots_[s].kind == ResourceKind::Texture ? device_.destroy(physical_[s].texture) : device_.destroy(physical_[s].buffer));
            created_[s] = false;
        }
        return first;
    }

private:
    rhi::Device& device_;
    const std::vector<PhysicalSlot>& slots_;
    std::vector<PhysicalResource> physical_;
    std::vector<bool> created_;
    std::unique_ptr<rhi::CommandList> list_;
    rhi::QueryPoolHandle pool_;
    rhi::FenceHandle fence_;
};

double to_ms(core::units::Seconds seconds) noexcept {
    return seconds.count() * 1000.0;
}

}  // namespace

PassContext::PassContext(rhi::CommandList& commands, const std::vector<ResourceAccess>& accesses,
                         const std::vector<std::uint32_t>& resource_slots, const std::vector<ResourceKind>& resource_kinds,
                         const std::vector<PhysicalResource>& physical) noexcept
    : commands_(&commands),
      accesses_(&accesses),
      resource_slots_(&resource_slots),
      resource_kinds_(&resource_kinds),
      physical_(&physical) {}

const PhysicalResource* PassContext::find(ResourceId resource, ResourceKind kind) const noexcept {
    if (!resource.valid() || resource.index >= resource_kinds_->size() || (*resource_kinds_)[resource.index] != kind) {
        return nullptr;
    }
    const bool declared = std::any_of(accesses_->begin(), accesses_->end(),
                                      [resource](const ResourceAccess& access) { return access.resource == resource; });
    if (!declared) {
        return nullptr;
    }
    return &(*physical_)[(*resource_slots_)[resource.index]];
}

rhi::TextureHandle PassContext::texture(ResourceId resource) const noexcept {
    const PhysicalResource* found = find(resource, ResourceKind::Texture);
    return found != nullptr ? found->texture : rhi::TextureHandle{};
}

rhi::BufferHandle PassContext::buffer(ResourceId resource) const noexcept {
    const PhysicalResource* found = find(resource, ResourceKind::Buffer);
    return found != nullptr ? found->buffer : rhi::BufferHandle{};
}

rhi::RhiResult<ExecutionReport> execute(rhi::Device& device, const CompiledGraph& graph) {
    using rhi::RhiResult;
    const std::size_t pass_count = graph.passes.size();
    const bool timestamps = pass_count > 0 && device.capabilities().enabled(rhi::Feature::TimestampQueries);
    const auto timestamp_count = static_cast<std::uint32_t>(pass_count * 2);
    if (timestamps && timestamp_count > rhi::kMaxQueryCount) {
        return rhi::fail<ExecutionReport>(rhi::RhiErrorCode::OutOfRange,
                                          "render graph: " + std::to_string(pass_count) + " passes need more timestamps than a query pool holds");
    }

    ExecutionResources resources(device, graph.slots);
    for (std::size_t s = 0; s < graph.slots.size(); ++s) {
        const rhi::RhiStatus status = create_slot(device, graph.slots[s], resources.physical()[s]);
        if (!status.ok()) {
            return rhi::fail<ExecutionReport>(status.error().code, status.error().message);
        }
        resources.mark_created(s);
    }

    if (timestamps) {
        auto pool = device.create_query_pool(timestamp_count);
        if (!pool.ok()) {
            return rhi::fail<ExecutionReport>(pool.error().code, pool.error().message);
        }
        resources.set_pool(pool.value());
    }
    auto list = device.create_command_list(rhi::QueueType::Graphics);
    if (!list.ok()) {
        return rhi::fail<ExecutionReport>(list.error().code, list.error().message);
    }
    resources.set_list(std::move(list.value()));
    rhi::CommandList& commands = resources.list();

    std::vector<std::uint32_t> resource_slots(graph.lifetimes.size(), 0);
    for (std::size_t r = 0; r < graph.lifetimes.size(); ++r) {
        resource_slots[r] = graph.lifetimes[r].slot;
    }
    std::vector<ResourceKind> resource_kinds;
    resource_kinds.reserve(graph.resources.size());
    for (const GraphResource& resource : graph.resources) {
        resource_kinds.push_back(resource.kind);
    }

    ExecutionReport report;
    report.backend = device.backend();
    report.passes.reserve(pass_count);
    const core::Stopwatch total;
    for (std::uint32_t i = 0; i < pass_count; ++i) {
        const CompiledPass& pass = graph.passes[i];
        const auto reject = [&pass](const rhi::RhiError& error) {
            return rhi::fail<ExecutionReport>(error.code, "pass '" + pass.name + "': " + error.message);
        };
        const core::Stopwatch cpu;
        if (timestamps) {
            if (auto status = commands.write_timestamp(resources.pool(), 2 * i); !status.ok()) {
                return reject(status.error());
            }
        }
        for (const Barrier& barrier : pass.barriers) {
            const PhysicalResource& physical = resources.physical()[barrier.slot];
            const rhi::RhiStatus status = graph.slots[barrier.slot].kind == ResourceKind::Texture
                                              ? commands.transition(physical.texture, barrier.before, barrier.after)
                                              : commands.transition(physical.buffer, barrier.before, barrier.after);
            if (!status.ok()) {
                return reject(status.error());
            }
        }
        if (pass.record) {
            PassContext context(commands, pass.accesses, resource_slots, resource_kinds, resources.physical());
            if (const rhi::RhiStatus status = pass.record(context); !status.ok()) {
                return reject(status.error());
            }
        }
        if (timestamps) {
            if (auto status = commands.write_timestamp(resources.pool(), 2 * i + 1); !status.ok()) {
                return reject(status.error());
            }
        }
        report.passes.push_back(PassTiming{pass.name, to_ms(cpu.elapsed()), std::nullopt});
        report.barriers += static_cast<std::uint32_t>(pass.barriers.size());
    }
    for (const Barrier& barrier : graph.exit_barriers) {
        const PhysicalResource& physical = resources.physical()[barrier.slot];
        const rhi::RhiStatus status = graph.slots[barrier.slot].kind == ResourceKind::Texture
                                          ? commands.transition(physical.texture, barrier.before, barrier.after)
                                          : commands.transition(physical.buffer, barrier.before, barrier.after);
        if (!status.ok()) {
            return rhi::fail<ExecutionReport>(status.error().code, status.error().message);
        }
        ++report.barriers;
    }
    if (const rhi::RhiStatus status = commands.close(); !status.ok()) {
        return rhi::fail<ExecutionReport>(status.error().code, status.error().message);
    }
    report.cpu_ms = to_ms(total.elapsed());

    auto fence = device.create_fence(0);
    if (!fence.ok()) {
        return rhi::fail<ExecutionReport>(fence.error().code, fence.error().message);
    }
    resources.set_fence(fence.value());
    std::vector<rhi::CommandList*> lists{&commands};
    if (const rhi::RhiStatus status = device.submit(rhi::QueueType::Graphics, lists, fence.value(), 1); !status.ok()) {
        return rhi::fail<ExecutionReport>(status.error().code, status.error().message);
    }
    if (const rhi::RhiStatus status = device.wait_fence(fence.value(), 1); !status.ok()) {
        return rhi::fail<ExecutionReport>(status.error().code, status.error().message);
    }

    // The timestamps are ready once the fence has passed. Without a clock frequency the ticks cannot become time, so the GPU
    // figures stay empty.
    const std::uint64_t frequency = device.capabilities().timestamp_frequency_hz;
    if (timestamps && frequency > 0) {
        const double ms_per_tick = 1000.0 / static_cast<double>(frequency);
        std::uint64_t first_begin = ~std::uint64_t{0};
        std::uint64_t last_end = 0;
        for (std::uint32_t i = 0; i < pass_count; ++i) {
            auto begin = device.read_timestamp(resources.pool(), 2 * i);
            if (!begin.ok()) {
                return rhi::fail<ExecutionReport>(begin.error().code, begin.error().message);
            }
            auto end = device.read_timestamp(resources.pool(), 2 * i + 1);
            if (!end.ok()) {
                return rhi::fail<ExecutionReport>(end.error().code, end.error().message);
            }
            report.passes[i].gpu_ms = static_cast<double>(end.value() - begin.value()) * ms_per_tick;
            first_begin = std::min(first_begin, begin.value());
            last_end = std::max(last_end, end.value());
        }
        if (pass_count > 0) {
            report.gpu_ms = static_cast<double>(last_end - first_begin) * ms_per_tick;
        }
    }

    std::uint32_t transient_resources = 0;
    std::uint32_t transient_slots = 0;
    for (std::size_t r = 0; r < graph.resources.size(); ++r) {
        if (!graph.resources[r].imported && graph.lifetimes[r].used) {
            ++transient_resources;
        }
    }
    for (const PhysicalSlot& slot : graph.slots) {
        if (!slot.imported) {
            ++transient_slots;
        }
    }
    report.transient_allocations = transient_slots;
    report.aliased_resources = transient_resources - transient_slots;

    if (rhi::RhiStatus status = resources.release(); !status.ok()) {
        return rhi::fail<ExecutionReport>(status.error().code, status.error().message);
    }
    return core::success(std::move(report));
}

}  // namespace engine::render
