#pragma once

#include "engine/rhi/error.hpp"
#include "engine/rhi/handle.hpp"
#include "engine/rhi/types.hpp"
#include "engine/rhi/upscaler.hpp"

#include <cstdint>

namespace engine::rhi {

// Records GPU work for one queue. A list is created open, recorded, closed with close(), and then submitted once. Every
// command validates its arguments when it is recorded and returns an error without changing the list if they are wrong.
//
// Recording order is submission order: the state a transition sees is the state left by every command recorded before it.
// A list is never reused after submission; create a new one for the next frame.
class CommandList {
public:
    virtual ~CommandList() = default;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;

    [[nodiscard]] virtual QueueType queue() const noexcept = 0;

    // Explicit state transitions. `before` must equal the tracked state, and `after` must be allowed by the usage flags.
    virtual RhiStatus transition(BufferHandle buffer, ResourceState before, ResourceState after) = 0;
    virtual RhiStatus transition(TextureHandle texture, ResourceState before, ResourceState after) = 0;

    virtual RhiStatus set_pipeline(PipelineHandle pipeline) = 0;
    // Binds the set at `set_index`. The set's layout must equal the pipeline's layout at that index.
    virtual RhiStatus set_descriptor_set(std::uint32_t set_index, DescriptorSetHandle set) = 0;
    virtual RhiStatus set_vertex_buffer(std::uint32_t slot, BufferHandle buffer) = 0;
    virtual RhiStatus set_index_buffer(BufferHandle buffer, IndexFormat format) = 0;

    virtual RhiStatus begin_render_pass(const RenderPassDesc& pass) = 0;
    virtual RhiStatus end_render_pass() = 0;

    virtual RhiStatus draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                                   std::int32_t base_vertex, std::uint32_t first_instance) = 0;
    virtual RhiStatus draw_indexed_indirect(BufferHandle args, std::uint64_t args_offset, std::uint32_t draw_count) = 0;
    // The GPU reads the draw count from `count` at execution time, up to `max_draw_count`. Needs the IndirectCount feature.
    virtual RhiStatus draw_indexed_indirect_count(BufferHandle args, std::uint64_t args_offset, BufferHandle count,
                                                  std::uint64_t count_offset, std::uint32_t max_draw_count) = 0;
    virtual RhiStatus dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;
    // Needs the MeshShaders feature and a bound Mesh pipeline.
    virtual RhiStatus dispatch_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;

    // Runs FSR 2 (US-046) on the inputs and writes the output. The state rules are on UpscaleDispatch. Fails with Unsupported on a
    // list whose device cannot run FSR 2, and with InvalidState inside a render pass. Clears the bound pipeline and sets.
    virtual RhiStatus upscale(Upscaler& upscaler, const UpscaleDispatch& dispatch) = 0;

    // Ray tracing (US-041). Needs the RayTracing feature and the graphics or compute queue; none of these run inside a render pass.
    // A build records the structure's geometry (or its instances, for a top-level structure) and marks the structure built. A
    // top-level build needs every instance's bottom-level structure built and alive.
    virtual RhiStatus build_bottom_level(AccelerationStructureHandle structure, BuildMode mode) = 0;
    virtual RhiStatus build_top_level(AccelerationStructureHandle structure, BuildMode mode) = 0;
    // Binds a ray tracing pipeline in place of a compute or graphics pipeline. Descriptor sets are then bound as for compute.
    virtual RhiStatus set_ray_tracing_pipeline(RayTracingPipelineHandle pipeline) = 0;
    // Launches width x height x depth rays. Every bound set must be written, and any top-level structure it names must be built.
    virtual RhiStatus dispatch_rays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) = 0;

    virtual RhiStatus copy_buffer(BufferHandle source, std::uint64_t source_offset, BufferHandle destination,
                                  std::uint64_t destination_offset, std::uint64_t size) = 0;
    // Copies mip 0 of `source` (CopySource state) into `destination` (CopyDestination state). Row i is written at
    // destination_offset + i * copy_row_pitch(width, format). destination_offset must be a multiple of
    // kTexturePlacementAlignment, and the buffer must hold the whole footprint. Depth formats are rejected.
    virtual RhiStatus copy_texture_to_buffer(TextureHandle source, BufferHandle destination, std::uint64_t destination_offset) = 0;
    // The reverse copy: mip 0 of `destination` (CopyDestination state) is filled from `source` (CopySource state). Row i is read
    // from source_offset + i * copy_row_pitch(width, format). source_offset must be a multiple of kTexturePlacementAlignment.
    virtual RhiStatus copy_buffer_to_texture(BufferHandle source, std::uint64_t source_offset, TextureHandle destination) = 0;
    // Stores a GPU timestamp at `index` when the submission executes. Read it with Device::read_timestamp after the fence.
    virtual RhiStatus write_timestamp(QueryPoolHandle pool, std::uint32_t index) = 0;

    virtual RhiStatus close() = 0;

protected:
    CommandList() = default;
};

}  // namespace engine::rhi
