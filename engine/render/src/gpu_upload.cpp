#include "gpu_upload.hpp"

#include "engine/core/result.hpp"
#include "engine/render/render_graph.hpp"

namespace engine::render::detail {

using rhi::BufferDesc;
using rhi::BufferHandle;
using rhi::BufferUsage;
using rhi::ResourceState;
using rhi::RhiErrorCode;
using rhi::RhiStatus;

rhi::BufferDesc gpu_buffer_desc(std::uint64_t size, rhi::BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return desc;
}

RhiStatus upload_buffer_through_graph(rhi::Device& device, BufferHandle buffer, BufferUsage usage, std::uint64_t size,
                                      const std::vector<std::uint8_t>& bytes, ResourceState before, ResourceState after) {
    const std::uint64_t copy = bytes.size();
    if (copy == 0 || copy > size) {
        return rhi::fail(RhiErrorCode::OutOfRange, "ray-traced upload: the data must be non-empty and fit the buffer");
    }
    const BufferDesc staging_desc = gpu_buffer_desc(copy, BufferUsage::CopySource | BufferUsage::CpuWrite, "ray-traced staging");
    rhi::RhiResult<BufferHandle> staging = device.create_buffer(staging_desc);
    if (!staging.ok()) {
        return core::failure(staging.error());
    }
    const BufferHandle staging_buffer = staging.value();
    if (const RhiStatus uploaded = device.upload_buffer(staging_buffer, 0, copy, bytes.data()); !uploaded.ok()) {
        static_cast<void>(device.destroy(staging_buffer));
        return uploaded;
    }

    RenderGraph graph;
    const ResourceId staging_res = graph.import_buffer("ray-traced staging", staging_buffer, staging_desc, ResourceState::CopySource, ResourceState::CopySource);
    const ResourceId buffer_res = graph.import_buffer("ray-traced buffer", buffer, gpu_buffer_desc(size, usage, "ray-traced buffer"), before, after);
    graph.add_pass("upload ray-traced buffer", PassKind::Graphics, {{staging_res, Access::CopySource}, {buffer_res, Access::CopyDestination}},
                   [=](PassContext& ctx) { return ctx.commands().copy_buffer(ctx.buffer(staging_res), 0, ctx.buffer(buffer_res), 0, copy); });
    const CompileResult compiled = graph.compile();
    if (!compiled.ok()) {
        static_cast<void>(device.destroy(staging_buffer));
        return rhi::fail(RhiErrorCode::InvalidState, "ray-traced upload: " + compiled.errors.front().message);
    }
    const rhi::RhiResult<ExecutionReport> executed = execute(device, *compiled.graph);
    static_cast<void>(device.destroy(staging_buffer));
    if (!executed.ok()) {
        return core::failure(executed.error());
    }
    return core::success();
}

}  // namespace engine::render::detail
