#pragma once

// Shared fixture for the render graph tests (US-032). The readback chain clears a target, copies it to a staging buffer, and downloads
// the staging buffer into a CPU-readable buffer. A second target with the same layout is cleared after the first one's last use, so
// the graph can reuse its allocation. The same graph runs on the CPU reference device and on the GPU backends.

#include "engine/render/render_graph.hpp"
#include "engine/rhi/command_list.hpp"
#include "engine/rhi/types.hpp"

#include <array>
#include <cstdint>

namespace engine::render::tests {

inline constexpr std::uint32_t kChainWidth = 64;
inline constexpr std::uint32_t kChainHeight = 64;
inline constexpr std::array<float, 4> kChainClear{0.25f, 0.5f, 0.75f, 1.0f};

inline rhi::TextureDesc chain_texture_desc(const char* name) {
    rhi::TextureDesc desc;
    desc.width = kChainWidth;
    desc.height = kChainHeight;
    desc.format = rhi::Format::R8G8B8A8Unorm;
    desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;
    desc.debug_name = name;
    return desc;
}

inline std::uint64_t chain_staging_size() {
    return rhi::copy_row_pitch(kChainWidth, rhi::Format::R8G8B8A8Unorm) * kChainHeight;
}

inline rhi::BufferDesc chain_staging_desc() {
    rhi::BufferDesc desc;
    desc.size_bytes = chain_staging_size();
    desc.usage = rhi::BufferUsage::CopyDestination | rhi::BufferUsage::CopySource;
    desc.debug_name = "graph staging";
    return desc;
}

inline rhi::BufferDesc chain_readback_desc() {
    rhi::BufferDesc desc;
    desc.size_bytes = chain_staging_size();
    desc.usage = rhi::BufferUsage::CpuRead | rhi::BufferUsage::CopyDestination;
    desc.debug_name = "graph readback";
    return desc;
}

// Passes: clear(target) -> stage(target -> staging) -> download(staging -> readback), then clear(scratch). The clear of scratch does
// not depend on the others, and it runs last in declaration order, so its lifetime starts after the target's ends.
inline RenderGraph build_chain_graph(rhi::BufferHandle readback) {
    RenderGraph graph;
    const ResourceId target = graph.create_texture("target", chain_texture_desc("graph target"));
    const ResourceId scratch = graph.create_texture("scratch", chain_texture_desc("graph scratch"));
    const ResourceId staging = graph.create_buffer("staging", chain_staging_desc());
    const ResourceId out = graph.import_buffer("readback", readback, chain_readback_desc(), rhi::ResourceState::CopyDestination);

    const auto clear = [](ResourceId resource) {
        return [resource](PassContext& ctx) {
            rhi::RenderPassDesc pass;
            pass.color_targets = {ctx.texture(resource)};
            pass.clear_color = kChainClear;
            if (const rhi::RhiStatus status = ctx.commands().begin_render_pass(pass); !status.ok()) {
                return status;
            }
            return ctx.commands().end_render_pass();
        };
    };
    graph.add_pass("clear", PassKind::Graphics, {{target, Access::ColorWrite}}, clear(target));
    graph.add_pass("stage", PassKind::Graphics, {{target, Access::CopySource}, {staging, Access::CopyDestination}},
                   [target, staging](PassContext& ctx) {
                       return ctx.commands().copy_texture_to_buffer(ctx.texture(target), ctx.buffer(staging), 0);
                   });
    graph.add_pass("download", PassKind::Compute, {{staging, Access::CopySource}, {out, Access::CopyDestination}},
                   [staging, out](PassContext& ctx) {
                       return ctx.commands().copy_buffer(ctx.buffer(staging), 0, ctx.buffer(out), 0, chain_staging_size());
                   });
    graph.add_pass("clear_scratch", PassKind::Graphics, {{scratch, Access::ColorWrite}}, clear(scratch));
    return graph;
}

}  // namespace engine::render::tests
