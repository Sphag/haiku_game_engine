#pragma once

#include "engine/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace engine::rhi {

struct ValidationDeviceDesc {
    Feature simulated_hardware = kAllFeatures;  // features the simulated adapter reports
    Feature disabled_features = Feature::None;  // forced off on top of the hardware; reported as DisabledForTest
    std::string adapter_name = "Validation (CPU reference)";
};

// The in-house CPU reference backend. It runs no GPU work. It enforces the full contract: handle generations, resource
// states, usage flags, command-list state, queue rules, feature gates, descriptor layouts, deferred destruction, and fence
// and timestamp ordering. Its submissions complete in order on one timeline, when wait_fence() or wait_idle() runs them.
//
// Timestamps are nanoseconds from device creation, so the frequency is 1 GHz.
class ValidationDevice final : public Device {
public:
    explicit ValidationDevice(const ValidationDeviceDesc& desc = {});
    ~ValidationDevice() override = default;

    [[nodiscard]] Backend backend() const noexcept override;
    [[nodiscard]] const CapabilityReport& capabilities() const noexcept override;

    RhiResult<BufferHandle> create_buffer(const BufferDesc& desc) override;
    RhiResult<TextureHandle> create_texture(const TextureDesc& desc) override;
    RhiResult<DescriptorSetLayoutHandle> create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) override;
    RhiResult<DescriptorSetHandle> create_descriptor_set(DescriptorSetLayoutHandle layout) override;
    RhiResult<PipelineHandle> create_pipeline(const PipelineDesc& desc) override;
    RhiResult<FenceHandle> create_fence(std::uint64_t initial_value) override;
    RhiResult<QueryPoolHandle> create_query_pool(std::uint32_t count) override;
    RhiResult<std::unique_ptr<CommandList>> create_command_list(QueueType queue) override;
    RhiResult<AccelerationStructureHandle> create_bottom_level(const BottomLevelDesc& desc) override;
    RhiResult<AccelerationStructureHandle> create_top_level(const TopLevelDesc& desc) override;
    RhiStatus write_top_level(AccelerationStructureHandle structure, const std::vector<TopLevelInstance>& instances) override;
    RhiResult<RayTracingPipelineHandle> create_ray_tracing_pipeline(const RayTracingPipelineDesc& desc) override;

    RhiStatus write_buffer(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, BufferHandle buffer) override;
    RhiStatus write_texture(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index, TextureHandle texture) override;
    RhiStatus write_acceleration_structure(DescriptorSetHandle set, std::uint32_t binding, std::uint32_t array_index,
                                           AccelerationStructureHandle structure) override;

    RhiStatus destroy(BufferHandle buffer) override;
    RhiStatus destroy(TextureHandle texture) override;
    RhiStatus destroy(DescriptorSetLayoutHandle layout) override;
    RhiStatus destroy(DescriptorSetHandle set) override;
    RhiStatus destroy(PipelineHandle pipeline) override;
    RhiStatus destroy(FenceHandle fence) override;
    RhiStatus destroy(QueryPoolHandle pool) override;
    RhiStatus destroy(AccelerationStructureHandle structure) override;
    RhiStatus destroy(RayTracingPipelineHandle pipeline) override;

    RhiStatus submit(QueueType queue, const std::vector<CommandList*>& lists, FenceHandle signal_fence,
                     std::uint64_t signal_value) override;
    RhiResult<std::uint64_t> fence_value(FenceHandle fence) override;
    RhiStatus wait_fence(FenceHandle fence, std::uint64_t value) override;
    RhiStatus wait_idle() override;
    RhiResult<std::uint64_t> read_timestamp(QueryPoolHandle pool, std::uint32_t index) override;
    RhiStatus read_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, void* destination) override;
    RhiStatus upload_buffer(BufferHandle buffer, std::uint64_t offset, std::uint64_t size, const void* source) override;
    RhiStatus queue_wait(QueueType queue, FenceHandle fence, std::uint64_t value) override;
    RhiResult<ResourceStats> resource_stats() override;
    RhiResult<std::unique_ptr<Swapchain>> create_swapchain(const SwapchainDesc& desc) override;
    RhiResult<std::unique_ptr<Upscaler>> create_upscaler(const UpscalerDesc& desc) override;
    RhiStatus check_debug_messages() override;

    // Diagnostics for tests: objects still allocated (including destroyed objects a submission still holds), and
    // submissions not yet executed.
    [[nodiscard]] std::size_t allocated_object_count() const noexcept;
    [[nodiscard]] std::size_t pending_submission_count() const noexcept;

private:
    class Recorder;
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace engine::rhi
