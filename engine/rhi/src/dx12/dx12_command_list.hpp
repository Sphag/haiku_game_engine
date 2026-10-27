#pragma once

#include "dx12/dx12_device.hpp"

#include "engine/rhi/command_list.hpp"

#include <array>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::rhi::dx12 {

// The D3D12 objects one list owns. Submission moves them into the device's retained set, so they outlive the CommandList that
// recorded them.
struct ListData {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

// Records one D3D12 command list. Every command checks the same rules as the CPU reference device: handle liveness, tracked
// states, usage flags, and render-pass nesting. Commands that need pipelines or descriptors fail with Unsupported until US-029.
class Dx12CommandList final : public CommandList {
public:
    Dx12CommandList(Dx12Device& device, QueueType queue, std::shared_ptr<ListData> data);
    ~Dx12CommandList() override = default;

    [[nodiscard]] QueueType queue() const noexcept override;

    RhiStatus transition(BufferHandle buffer, ResourceState before, ResourceState after) override;
    RhiStatus transition(TextureHandle texture, ResourceState before, ResourceState after) override;

    RhiStatus set_pipeline(PipelineHandle pipeline) override;
    RhiStatus set_descriptor_set(std::uint32_t set_index, DescriptorSetHandle set) override;
    RhiStatus set_vertex_buffer(std::uint32_t slot, BufferHandle buffer) override;
    RhiStatus set_index_buffer(BufferHandle buffer, IndexFormat format) override;

    RhiStatus begin_render_pass(const RenderPassDesc& pass) override;
    RhiStatus end_render_pass() override;

    RhiStatus draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                           std::int32_t base_vertex, std::uint32_t first_instance) override;
    RhiStatus draw_indexed_indirect(BufferHandle args, std::uint64_t args_offset, std::uint32_t draw_count) override;
    RhiStatus draw_indexed_indirect_count(BufferHandle args, std::uint64_t args_offset, BufferHandle count,
                                          std::uint64_t count_offset, std::uint32_t max_draw_count) override;
    RhiStatus dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    RhiStatus dispatch_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    RhiStatus upscale(Upscaler& upscaler, const UpscaleDispatch& dispatch) override;

    RhiStatus build_bottom_level(AccelerationStructureHandle structure, BuildMode mode) override;
    RhiStatus build_top_level(AccelerationStructureHandle structure, BuildMode mode) override;
    RhiStatus set_ray_tracing_pipeline(RayTracingPipelineHandle pipeline) override;
    RhiStatus dispatch_rays(std::uint32_t width, std::uint32_t height, std::uint32_t depth) override;

    RhiStatus copy_buffer(BufferHandle source, std::uint64_t source_offset, BufferHandle destination,
                          std::uint64_t destination_offset, std::uint64_t size) override;
    RhiStatus copy_texture_to_buffer(TextureHandle source, BufferHandle destination, std::uint64_t destination_offset) override;
    RhiStatus copy_buffer_to_texture(BufferHandle source, std::uint64_t source_offset, TextureHandle destination) override;
    RhiStatus write_timestamp(QueryPoolHandle pool, std::uint32_t index) override;

    RhiStatus close() override;

private:
    friend class Dx12Device;

    enum class ListState : std::uint8_t { Recording, Closed, Submitted };

    [[nodiscard]] RhiStatus require_recording(std::string_view operation) const;
    // The layouts of the bound pipeline: a ray tracing pipeline's when one is bound, otherwise the graphics or compute pipeline's.
    [[nodiscard]] const std::vector<std::vector<DescriptorBindingDesc>>& active_layouts() const;
    [[nodiscard]] RhiStatus check_bound_sets(std::string_view operation);
    // Binds slot 0, slot 1 when the pipeline reads per-instance input, and the index buffer.
    [[nodiscard]] RhiStatus bind_input(std::string_view operation);
    [[nodiscard]] RhiStatus execute_indexed(BufferHandle args, std::uint64_t args_offset, BufferHandle count_buffer, std::uint64_t count_offset,
                                            std::uint32_t max_draws, std::string_view operation);
    void use(ObjectBase& object);

    Dx12Device& device_;
    QueueType queue_;
    std::shared_ptr<ListData> data_;
    ListState state_ = ListState::Recording;
    bool pass_active_ = false;
    std::vector<std::shared_ptr<ObjectBase>> refs_;  // each object once per use; deduplicated at submit
    std::vector<std::pair<std::shared_ptr<QueryPoolObject>, std::uint32_t>> timestamps_;

    // Compute state (US-029). The list holds the bound pipeline and sets, so a destroyed one stays valid until submission.
    std::shared_ptr<PipelineObject> pipeline_;
    std::array<std::shared_ptr<DescriptorSetObject>, kMaxDescriptorSetsPerPipeline> sets_{};
    std::uint32_t bound_set_mask_ = 0;

    // Ray tracing state (US-041). A bound ray tracing pipeline replaces pipeline_; the list holds it so it stays alive until submission.
    std::shared_ptr<RayTracingPipelineObject> ray_pipeline_;

    // Graphics state (US-033). The list holds the vertex and index buffers it binds, so a destroyed one stays valid until submission.
    std::shared_ptr<BufferObject> vertex_buffer_;
    std::shared_ptr<BufferObject> instance_buffer_;  // slot 1, per-instance input
    std::shared_ptr<BufferObject> index_buffer_;
    IndexFormat index_format_ = IndexFormat::Uint32;
};

}  // namespace engine::rhi::dx12
