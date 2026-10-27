// In-house GPU contract (US-026). These run the contract on the CPU reference device, so they pass on every operating
// system without a GPU. Each test checks one rule the contract states: lifetime, explicit transitions, queues, feature
// gates, descriptors, fences, and timestamps.

#include "engine/rhi/capabilities.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/error.hpp"
#include "engine/rhi/validation_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using engine::rhi::AccelerationStructureHandle;
using engine::rhi::BottomLevelDesc;
using engine::rhi::BuildMode;
using engine::rhi::RayTracingPipelineDesc;
using engine::rhi::RayTracingPipelineHandle;
using engine::rhi::TopLevelDesc;
using engine::rhi::BufferDesc;
using engine::rhi::bytes_per_pixel;
using engine::rhi::BufferHandle;
using engine::rhi::BufferUsage;
using engine::rhi::CapabilityReport;
using engine::rhi::CommandList;
using engine::rhi::DescriptorBindingDesc;
using engine::rhi::DescriptorSetHandle;
using engine::rhi::DescriptorSetLayoutDesc;
using engine::rhi::DescriptorSetLayoutHandle;
using engine::rhi::DescriptorType;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::Feature;
using engine::rhi::FeatureState;
using engine::rhi::FenceHandle;
using engine::rhi::Format;
using engine::rhi::IndexFormat;
using engine::rhi::PipelineDesc;
using engine::rhi::PipelineHandle;
using engine::rhi::PipelineKind;
using engine::rhi::QueryPoolHandle;
using engine::rhi::QueueType;
using engine::rhi::RenderPassDesc;
using engine::rhi::ResourceState;
using engine::rhi::ResourceStats;
using engine::rhi::RhiErrorCode;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::TextureDesc;
using engine::rhi::TextureHandle;
using engine::rhi::TextureUsage;
using engine::rhi::ValidationDevice;
using engine::rhi::ValidationDeviceDesc;
using engine::rhi::SwapchainDesc;

namespace {

// Unwraps a successful result, or fails the test with the error message.
template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

void require_ok(const RhiStatus& status) {
    if (!status.ok()) {
        FAIL(status.error().message);
    }
}

template <typename T>
bool fails_with(const RhiResult<T>& result, RhiErrorCode code) {
    return !result.ok() && result.error().code == code;
}

BufferHandle make_buffer(Device& device, std::uint64_t size, BufferUsage usage) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    return take(device.create_buffer(desc));
}

TextureHandle make_color(Device& device) {
    TextureDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::R8G8B8A8Unorm;
    desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    return take(device.create_texture(desc));
}

PipelineHandle make_graphics_pipeline(Device& device, std::vector<DescriptorSetLayoutHandle> layouts = {},
                                      Format color = Format::R8G8B8A8Unorm) {
    PipelineDesc desc;
    desc.kind = PipelineKind::Graphics;
    desc.shaders = {ShaderBinary{ShaderStage::Vertex, "main", {1, 2, 3}}, ShaderBinary{ShaderStage::Pixel, "main", {4}}};
    desc.set_layouts = std::move(layouts);
    desc.color_formats = {color};
    return take(device.create_pipeline(desc));
}

// Records a complete frame that draws one indexed triangle into `target`. Leaves the list open.
std::unique_ptr<CommandList> record_triangle(Device& device, BufferHandle vertices, BufferHandle indices, TextureHandle target,
                                             PipelineHandle pipeline) {
    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->transition(indices, ResourceState::Common, ResourceState::IndexBuffer));
    require_ok(list->transition(target, ResourceState::Common, ResourceState::RenderTarget));
    require_ok(list->set_pipeline(pipeline));
    require_ok(list->set_vertex_buffer(0, vertices));
    require_ok(list->set_index_buffer(indices, IndexFormat::Uint32));
    RenderPassDesc pass;
    pass.color_targets = {target};
    require_ok(list->begin_render_pass(pass));
    require_ok(list->draw_indexed(3, 1, 0, 0, 0));
    require_ok(list->end_render_pass());
    return list;
}

}  // namespace

TEST_CASE("A default handle is invalid and a handle from another device is rejected", "[unit]") {
    engine::rhi::BufferHandle none;
    CHECK_FALSE(none.valid());

    ValidationDevice first;
    ValidationDevice second;
    const BufferHandle buffer = make_buffer(first, 64, BufferUsage::Vertex);
    CHECK(buffer.valid());

    CHECK(fails_with(second.destroy(buffer), RhiErrorCode::InvalidHandle));
    CHECK(first.destroy(buffer).ok());
}

TEST_CASE("A destroyed handle is rejected even after its slot is reused", "[unit]") {
    ValidationDevice device;
    const BufferHandle old_buffer = make_buffer(device, 64, BufferUsage::Vertex);
    require_ok(device.destroy(old_buffer));

    const BufferHandle reused = make_buffer(device, 64, BufferUsage::Vertex);
    CHECK(reused.index == old_buffer.index);
    CHECK(reused.generation != old_buffer.generation);

    CHECK(fails_with(device.destroy(old_buffer), RhiErrorCode::InvalidHandle));
    auto list = take(device.create_command_list(QueueType::Graphics));
    CHECK(fails_with(list->transition(old_buffer, ResourceState::Common, ResourceState::VertexBuffer), RhiErrorCode::InvalidHandle));
    require_ok(list->transition(reused, ResourceState::Common, ResourceState::VertexBuffer));
}

TEST_CASE("Buffer and texture descriptions are validated when created", "[unit]") {
    ValidationDevice device;

    BufferDesc empty_size;
    empty_size.usage = BufferUsage::Vertex;
    CHECK(fails_with(device.create_buffer(empty_size), RhiErrorCode::InvalidDescriptor));

    BufferDesc no_usage;
    no_usage.size_bytes = 16;
    CHECK(fails_with(device.create_buffer(no_usage), RhiErrorCode::InvalidDescriptor));

    TextureDesc base;
    base.width = 64;
    base.height = 64;
    base.format = Format::R8G8B8A8Unorm;
    base.usage = TextureUsage::ShaderResource;

    TextureDesc too_many_mips = base;
    too_many_mips.mip_levels = 8;  // 64 -> 1 has 7 levels
    CHECK(fails_with(device.create_texture(too_many_mips), RhiErrorCode::OutOfRange));

    TextureDesc full_chain = base;
    full_chain.mip_levels = 7;
    CHECK(device.create_texture(full_chain).ok());

    TextureDesc depth_without_depth_format = base;
    depth_without_depth_format.usage = TextureUsage::DepthStencil;
    CHECK(fails_with(device.create_texture(depth_without_depth_format), RhiErrorCode::InvalidDescriptor));

    TextureDesc render_target_in_depth = base;
    render_target_in_depth.format = Format::D32Float;
    render_target_in_depth.usage = TextureUsage::RenderTarget;
    CHECK(fails_with(device.create_texture(render_target_in_depth), RhiErrorCode::InvalidDescriptor));
}

TEST_CASE("A transition must name the tracked state and only states its usage allows", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 64, BufferUsage::Vertex);
    auto list = take(device.create_command_list(QueueType::Graphics));

    // The buffer starts in Common, so a transition that names another state is rejected.
    CHECK(fails_with(list->transition(vertices, ResourceState::VertexBuffer, ResourceState::Common), RhiErrorCode::StateMismatch));
    // A Vertex-only buffer cannot enter the constant-buffer state.
    CHECK(fails_with(list->transition(vertices, ResourceState::Common, ResourceState::ConstantBuffer), RhiErrorCode::InvalidDescriptor));

    // Rejected commands change nothing, so the correct transition still works.
    require_ok(list->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->transition(vertices, ResourceState::VertexBuffer, ResourceState::Common));
}

TEST_CASE("A recorded frame submits, completes on its fence, and resolves its timestamps", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 256, BufferUsage::Vertex);
    const BufferHandle indices = make_buffer(device, 256, BufferUsage::Index);
    const TextureHandle target = make_color(device);
    const PipelineHandle pipeline = make_graphics_pipeline(device);
    const FenceHandle fence = take(device.create_fence(0));
    const QueryPoolHandle timestamps = take(device.create_query_pool(2));

    auto list = record_triangle(device, vertices, indices, target, pipeline);
    require_ok(list->write_timestamp(timestamps, 0));
    require_ok(list->write_timestamp(timestamps, 1));
    require_ok(list->close());

    const std::vector<CommandList*> lists{list.get()};
    require_ok(device.submit(QueueType::Graphics, lists, fence, 1));
    CHECK(device.pending_submission_count() == 1);
    CHECK(take(device.fence_value(fence)) == 0);
    CHECK(fails_with(device.read_timestamp(timestamps, 0), RhiErrorCode::NotReady));

    require_ok(device.wait_fence(fence, 1));
    CHECK(device.pending_submission_count() == 0);
    CHECK(take(device.fence_value(fence)) == 1);

    const std::uint64_t begin = take(device.read_timestamp(timestamps, 0));
    const std::uint64_t end = take(device.read_timestamp(timestamps, 1));
    CHECK(end >= begin);
    CHECK(device.capabilities().timestamp_frequency_hz > 0);
}

TEST_CASE("Draws need their vertex and index buffers in the states the bindings name", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 256, BufferUsage::Vertex);
    const BufferHandle indices = make_buffer(device, 256, BufferUsage::Index);
    const TextureHandle target = make_color(device);
    const PipelineHandle pipeline = make_graphics_pipeline(device);

    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(target, ResourceState::Common, ResourceState::RenderTarget));
    require_ok(list->set_pipeline(pipeline));
    require_ok(list->set_vertex_buffer(0, vertices));
    require_ok(list->set_index_buffer(indices, IndexFormat::Uint32));
    RenderPassDesc pass;
    pass.color_targets = {target};
    require_ok(list->begin_render_pass(pass));
    // Neither buffer was transitioned, so the draw is rejected.
    CHECK(fails_with(list->draw_indexed(3, 1, 0, 0, 0), RhiErrorCode::StateMismatch));
    // A draw without an index buffer is rejected even in the right states.
    // Tracked state is shared by every list in recording order, so this list needs a target that is still in Common.
    const TextureHandle second_target = make_color(device);
    auto no_index = take(device.create_command_list(QueueType::Graphics));
    require_ok(no_index->transition(second_target, ResourceState::Common, ResourceState::RenderTarget));
    require_ok(no_index->set_pipeline(pipeline));
    require_ok(no_index->set_vertex_buffer(0, vertices));
    RenderPassDesc second_pass;
    second_pass.color_targets = {second_target};
    require_ok(no_index->begin_render_pass(second_pass));
    CHECK(fails_with(no_index->draw_indexed(3, 1, 0, 0, 0), RhiErrorCode::InvalidState));
}

TEST_CASE("Command lists are rejected once closed, and a submission must use a closed list of its queue", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 64, BufferUsage::Vertex);
    const FenceHandle fence = take(device.create_fence(0));

    auto open = take(device.create_command_list(QueueType::Graphics));
    require_ok(open->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    CHECK(fails_with(device.submit(QueueType::Graphics, {open.get()}, fence, 1), RhiErrorCode::InvalidState));

    require_ok(open->close());
    CHECK(fails_with(open->transition(vertices, ResourceState::VertexBuffer, ResourceState::Common), RhiErrorCode::InvalidState));
    CHECK(fails_with(device.submit(QueueType::Compute, {open.get()}, fence, 1), RhiErrorCode::InvalidState));

    require_ok(device.submit(QueueType::Graphics, {open.get()}, fence, 1));
    CHECK(fails_with(device.submit(QueueType::Graphics, {open.get()}, fence, 2), RhiErrorCode::InvalidState));
    require_ok(device.wait_idle());
}

TEST_CASE("A rejected submission changes no list, so the valid lists can still be submitted", "[unit]") {
    ValidationDevice device;
    const FenceHandle fence = take(device.create_fence(0));

    auto good = take(device.create_command_list(QueueType::Graphics));
    require_ok(good->close());
    auto open = take(device.create_command_list(QueueType::Graphics));

    CHECK(fails_with(device.submit(QueueType::Graphics, {good.get(), open.get()}, fence, 1), RhiErrorCode::InvalidState));
    CHECK(device.pending_submission_count() == 0);
    require_ok(device.submit(QueueType::Graphics, {good.get()}, fence, 1));
    require_ok(device.wait_idle());
}

TEST_CASE("A resource destroyed after recording is rejected at submission", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 64, BufferUsage::Vertex);
    const FenceHandle fence = take(device.create_fence(0));

    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->close());
    require_ok(device.destroy(vertices));

    CHECK(fails_with(device.submit(QueueType::Graphics, {list.get()}, fence, 1), RhiErrorCode::InvalidHandle));
    CHECK(device.pending_submission_count() == 0);
}

TEST_CASE("Destroying an in-flight resource invalidates the handle at once and frees storage after its fence", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 64, BufferUsage::Vertex);
    const FenceHandle fence = take(device.create_fence(0));
    const std::size_t allocated = device.allocated_object_count();  // the buffer and the fence

    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->close());
    require_ok(device.submit(QueueType::Graphics, {list.get()}, fence, 1));

    require_ok(device.destroy(vertices));
    CHECK(fails_with(device.destroy(vertices), RhiErrorCode::InvalidHandle));
    // The submission still holds the buffer, so its storage stays allocated until the fence completes.
    CHECK(device.allocated_object_count() == allocated);

    require_ok(device.wait_fence(fence, 1));
    CHECK(device.allocated_object_count() == allocated - 1);
}

TEST_CASE("A fence reports the value it completed and cannot be waited on for an unreachable value", "[unit]") {
    ValidationDevice device;
    const FenceHandle fence = take(device.create_fence(5));
    CHECK(take(device.fence_value(fence)) == 5);

    CHECK(fails_with(device.wait_fence(fence, 6), RhiErrorCode::InvalidState));
    require_ok(device.wait_fence(fence, 5));

    auto empty = take(device.create_command_list(QueueType::Compute));
    require_ok(empty->close());
    require_ok(device.submit(QueueType::Compute, {empty.get()}, fence, 9));
    require_ok(device.wait_fence(fence, 9));
    CHECK(take(device.fence_value(fence)) == 9);
}

TEST_CASE("Timestamps are rejected out of range and before their submission completes", "[unit]") {
    ValidationDevice device;
    const QueryPoolHandle pool = take(device.create_query_pool(2));
    const FenceHandle fence = take(device.create_fence(0));

    CHECK(fails_with(device.create_query_pool(0), RhiErrorCode::OutOfRange));
    CHECK(fails_with(device.read_timestamp(pool, 0), RhiErrorCode::NotReady));
    CHECK(fails_with(device.read_timestamp(pool, 2), RhiErrorCode::OutOfRange));

    auto list = take(device.create_command_list(QueueType::Graphics));
    CHECK(fails_with(list->write_timestamp(pool, 2), RhiErrorCode::OutOfRange));
    require_ok(list->write_timestamp(pool, 0));
    require_ok(list->close());
    require_ok(device.submit(QueueType::Graphics, {list.get()}, fence, 1));
    CHECK(fails_with(device.read_timestamp(pool, 0), RhiErrorCode::NotReady));
    require_ok(device.wait_fence(fence, 1));
    CHECK(device.read_timestamp(pool, 0).ok());
}

TEST_CASE("Mesh shaders can be forcibly disabled and are then reported as not enabled", "[unit]") {
    ValidationDeviceDesc desc;
    desc.disabled_features = Feature::MeshShaders;
    ValidationDevice device(desc);
    const CapabilityReport& report = device.capabilities();

    CHECK(report.state(Feature::MeshShaders) == FeatureState::DisabledForTest);
    CHECK_FALSE(report.enabled(Feature::MeshShaders));
    CHECK(report.enabled(Feature::IndirectCount));
    CHECK(report.enabled(Feature::RayTracing));

    PipelineDesc mesh;
    mesh.kind = PipelineKind::Mesh;
    mesh.shaders = {ShaderBinary{ShaderStage::Mesh, "main", {1}}, ShaderBinary{ShaderStage::Pixel, "main", {2}}};
    CHECK(fails_with(device.create_pipeline(mesh), RhiErrorCode::FeatureDisabled));
}

TEST_CASE("Unsupported and forced-off features are reported differently", "[unit]") {
    ValidationDeviceDesc desc;
    desc.simulated_hardware = Feature::IndirectCount | Feature::TimestampQueries;
    desc.disabled_features = Feature::IndirectCount;
    ValidationDevice device(desc);
    const CapabilityReport& report = device.capabilities();

    CHECK(report.state(Feature::MeshShaders) == FeatureState::Unsupported);
    CHECK(report.state(Feature::RayTracing) == FeatureState::Unsupported);
    CHECK(report.state(Feature::IndirectCount) == FeatureState::DisabledForTest);
    CHECK(report.state(Feature::TimestampQueries) == FeatureState::Supported);
    CHECK_FALSE(report.enabled(Feature::RayTracing));
}

TEST_CASE("Feature-gated commands fail with FeatureDisabled when their feature is off", "[unit]") {
    ValidationDeviceDesc desc;
    desc.disabled_features = Feature::IndirectCount | Feature::MeshShaders | Feature::TimestampQueries;
    ValidationDevice device(desc);
    const BufferHandle args = make_buffer(device, 64, BufferUsage::IndirectArgument);
    const BufferHandle count = make_buffer(device, 4, BufferUsage::IndirectArgument);
    const QueryPoolHandle pool = take(device.create_query_pool(1));

    auto list = take(device.create_command_list(QueueType::Graphics));
    CHECK(fails_with(list->draw_indexed_indirect_count(args, 0, count, 0, 1), RhiErrorCode::FeatureDisabled));
    CHECK(fails_with(list->dispatch_mesh(1, 1, 1), RhiErrorCode::FeatureDisabled));
    CHECK(fails_with(list->write_timestamp(pool, 0), RhiErrorCode::FeatureDisabled));
}

TEST_CASE("The factory creates the validation device and reports unbuilt backends as unsupported", "[unit]") {
    DeviceDesc validation;
    validation.disabled_features = Feature::MeshShaders;
    auto device = take(engine::rhi::create_device(validation));
    CHECK(device->backend() == engine::rhi::Backend::Validation);
    CHECK(device->capabilities().state(Feature::MeshShaders) == FeatureState::DisabledForTest);

    // The DX12 backend exists only on Windows, where it creates a real device. Its behavior is tested by the GPU suite.
#ifndef _WIN32
    DeviceDesc dx12;
    dx12.backend = engine::rhi::Backend::Dx12;
    CHECK(fails_with(engine::rhi::create_device(dx12), RhiErrorCode::Unsupported));
#endif

    // The Vulkan backend is built on both OSes. Creating it needs a loader and a hardware adapter, so it may fail, but a failure is
    // Unsupported and a success is a Vulkan device: the factory never falls back to another backend.
    DeviceDesc vulkan;
    vulkan.backend = engine::rhi::Backend::Vulkan;
    const auto created = engine::rhi::create_device(vulkan);
    if (created.ok()) {
        CHECK(created.value()->backend() == engine::rhi::Backend::Vulkan);
    } else {
        CHECK(created.error().code == RhiErrorCode::Unsupported);
    }
}

TEST_CASE("Pipelines must have the stages of their kind", "[unit]") {
    ValidationDevice device;

    PipelineDesc graphics_without_pixel;
    graphics_without_pixel.kind = PipelineKind::Graphics;
    graphics_without_pixel.shaders = {ShaderBinary{ShaderStage::Vertex, "main", {1}}};
    CHECK(fails_with(device.create_pipeline(graphics_without_pixel), RhiErrorCode::InvalidDescriptor));

    PipelineDesc repeated_stage;
    repeated_stage.kind = PipelineKind::Compute;
    repeated_stage.shaders = {ShaderBinary{ShaderStage::Compute, "main", {1}}, ShaderBinary{ShaderStage::Compute, "main", {2}}};
    CHECK(fails_with(device.create_pipeline(repeated_stage), RhiErrorCode::InvalidDescriptor));

    PipelineDesc compute;
    compute.kind = PipelineKind::Compute;
    compute.shaders = {ShaderBinary{ShaderStage::Compute, "main", {1}}};
    CHECK(device.create_pipeline(compute).ok());
}

TEST_CASE("Descriptor writes match the binding type and array bounds", "[unit]") {
    ValidationDevice device;
    DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings = {DescriptorBindingDesc{0, DescriptorType::ConstantBuffer, 2, ShaderStage::Pixel}};
    const DescriptorSetLayoutHandle layout = take(device.create_descriptor_set_layout(layout_desc));
    const DescriptorSetHandle set = take(device.create_descriptor_set(layout));

    const TextureHandle texture = make_color(device);
    const BufferHandle constants = make_buffer(device, 256, BufferUsage::Constant);
    const BufferHandle plain = make_buffer(device, 256, BufferUsage::Vertex);

    CHECK(fails_with(device.write_texture(set, 0, 0, texture), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(device.write_buffer(set, 0, 0, plain), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(device.write_buffer(set, 0, 2, constants), RhiErrorCode::OutOfRange));
    CHECK(fails_with(device.write_buffer(set, 7, 0, constants), RhiErrorCode::InvalidDescriptor));
    require_ok(device.write_buffer(set, 0, 1, constants));

    DescriptorSetLayoutDesc repeated;
    repeated.bindings = {DescriptorBindingDesc{3, DescriptorType::ConstantBuffer, 1, ShaderStage::Pixel},
                         DescriptorBindingDesc{3, DescriptorType::ShaderResource, 1, ShaderStage::Pixel}};
    CHECK(fails_with(device.create_descriptor_set_layout(repeated), RhiErrorCode::InvalidDescriptor));
}

TEST_CASE("A descriptor set must match the pipeline layout and its resources must be in the state it needs", "[unit]") {
    ValidationDevice device;
    DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings = {DescriptorBindingDesc{0, DescriptorType::ConstantBuffer, 1, ShaderStage::Compute}};
    const DescriptorSetLayoutHandle layout = take(device.create_descriptor_set_layout(layout_desc));
    DescriptorSetLayoutDesc other_desc;
    other_desc.bindings = {DescriptorBindingDesc{0, DescriptorType::ShaderResource, 1, ShaderStage::Compute}};
    const DescriptorSetLayoutHandle other_layout = take(device.create_descriptor_set_layout(other_desc));

    PipelineDesc compute;
    compute.kind = PipelineKind::Compute;
    compute.shaders = {ShaderBinary{ShaderStage::Compute, "main", {1}}};
    compute.set_layouts = {layout};
    const PipelineHandle pipeline = take(device.create_pipeline(compute));

    const DescriptorSetHandle set = take(device.create_descriptor_set(layout));
    const DescriptorSetHandle wrong = take(device.create_descriptor_set(other_layout));
    const BufferHandle constants = make_buffer(device, 256, BufferUsage::Constant);
    require_ok(device.write_buffer(set, 0, 0, constants));

    auto list = take(device.create_command_list(QueueType::Compute));
    require_ok(list->set_pipeline(pipeline));
    CHECK(fails_with(list->set_descriptor_set(0, wrong), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(list->set_descriptor_set(1, set), RhiErrorCode::OutOfRange));
    require_ok(list->set_descriptor_set(0, set));
    // The buffer is still in Common, but its binding reads it as a constant buffer.
    CHECK(fails_with(list->dispatch(1, 1, 1), RhiErrorCode::StateMismatch));
    require_ok(list->transition(constants, ResourceState::Common, ResourceState::ConstantBuffer));
    require_ok(list->dispatch(1, 1, 1));
    require_ok(list->close());
}

TEST_CASE("Draw targets must match the pipeline formats and render passes cannot be nested", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 256, BufferUsage::Vertex);
    const BufferHandle indices = make_buffer(device, 256, BufferUsage::Index);
    const TextureHandle target = make_color(device);
    const PipelineHandle wide = make_graphics_pipeline(device, {}, Format::R16G16B16A16Float);

    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->transition(indices, ResourceState::Common, ResourceState::IndexBuffer));
    require_ok(list->transition(target, ResourceState::Common, ResourceState::RenderTarget));
    require_ok(list->set_pipeline(wide));
    require_ok(list->set_vertex_buffer(0, vertices));
    require_ok(list->set_index_buffer(indices, IndexFormat::Uint32));
    RenderPassDesc pass;
    pass.color_targets = {target};
    require_ok(list->begin_render_pass(pass));
    CHECK(fails_with(list->begin_render_pass(pass), RhiErrorCode::InvalidState));
    CHECK(fails_with(list->draw_indexed(3, 1, 0, 0, 0), RhiErrorCode::InvalidDescriptor));
    require_ok(list->end_render_pass());
    CHECK(fails_with(list->end_render_pass(), RhiErrorCode::InvalidState));
}

TEST_CASE("Indirect count reads its count and arguments from buffers in the indirect state", "[unit]") {
    ValidationDevice device;
    const BufferHandle vertices = make_buffer(device, 256, BufferUsage::Vertex);
    const BufferHandle indices = make_buffer(device, 256, BufferUsage::Index);
    const TextureHandle target = make_color(device);
    const PipelineHandle pipeline = make_graphics_pipeline(device);
    const BufferHandle args = make_buffer(device, 40, BufferUsage::IndirectArgument);
    const BufferHandle count = make_buffer(device, 4, BufferUsage::IndirectArgument);

    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->transition(indices, ResourceState::Common, ResourceState::IndexBuffer));
    require_ok(list->transition(target, ResourceState::Common, ResourceState::RenderTarget));
    require_ok(list->transition(args, ResourceState::Common, ResourceState::IndirectArgument));
    require_ok(list->transition(count, ResourceState::Common, ResourceState::IndirectArgument));
    require_ok(list->set_pipeline(pipeline));
    require_ok(list->set_vertex_buffer(0, vertices));
    require_ok(list->set_index_buffer(indices, IndexFormat::Uint32));
    RenderPassDesc pass;
    pass.color_targets = {target};
    require_ok(list->begin_render_pass(pass));

    // Two draw records need 40 bytes; a third would overrun the buffer.
    CHECK(fails_with(list->draw_indexed_indirect_count(args, 0, count, 0, 3), RhiErrorCode::OutOfRange));
    CHECK(fails_with(list->draw_indexed_indirect_count(args, 0, count, 2, 2), RhiErrorCode::OutOfRange));
    CHECK(fails_with(list->draw_indexed_indirect_count(args, 0, count, 0, 0), RhiErrorCode::OutOfRange));
    require_ok(list->draw_indexed_indirect_count(args, 0, count, 0, 2));
    require_ok(list->draw_indexed_indirect(args, 0, 2));
    require_ok(list->end_render_pass());
    require_ok(list->close());
}

TEST_CASE("Buffer copies check their ranges and reject copies within one buffer", "[unit]") {
    ValidationDevice device;
    const BufferHandle source = make_buffer(device, 64, BufferUsage::CopySource);
    const BufferHandle destination = make_buffer(device, 64, BufferUsage::CopyDestination);

    auto list = take(device.create_command_list(QueueType::Copy));
    require_ok(list->transition(source, ResourceState::Common, ResourceState::CopySource));
    require_ok(list->transition(destination, ResourceState::Common, ResourceState::CopyDestination));
    CHECK(fails_with(list->copy_buffer(source, 60, destination, 0, 8), RhiErrorCode::OutOfRange));
    CHECK(fails_with(list->copy_buffer(source, 0, destination, 0, 0), RhiErrorCode::OutOfRange));
    CHECK(fails_with(list->copy_buffer(source, 0, source, 0, 8), RhiErrorCode::InvalidDescriptor));
    require_ok(list->copy_buffer(source, 56, destination, 0, 8));
    require_ok(list->close());
}

TEST_CASE("A destroyed pipeline or descriptor set cannot be bound", "[unit]") {
    ValidationDevice device;
    const PipelineHandle pipeline = make_graphics_pipeline(device);
    require_ok(device.destroy(pipeline));

    auto list = take(device.create_command_list(QueueType::Graphics));
    CHECK(fails_with(list->set_pipeline(pipeline), RhiErrorCode::InvalidHandle));
}

TEST_CASE("Row pitch and bytes per texel follow the copy alignment rules", "[unit]") {
    CHECK(bytes_per_pixel(Format::R8G8B8A8Unorm) == 4);
    CHECK(bytes_per_pixel(Format::R16G16B16A16Float) == 8);
    CHECK(bytes_per_pixel(Format::D32Float) == 0);
    CHECK(engine::rhi::copy_row_pitch(64, Format::R8G8B8A8Unorm) == 256);
    CHECK(engine::rhi::copy_row_pitch(65, Format::R8G8B8A8Unorm) == 512);
    CHECK(engine::rhi::copy_row_pitch(1, Format::R8G8B8A8Unorm) == 256);
}

TEST_CASE("A CPU-readable buffer must be copy-destination, starts there, and allows no other state", "[unit]") {
    ValidationDevice device;
    BufferDesc alone;
    alone.size_bytes = 64;
    alone.usage = BufferUsage::CpuRead;
    CHECK(fails_with(device.create_buffer(alone), RhiErrorCode::InvalidDescriptor));

    const BufferHandle readback = make_buffer(device, 64, BufferUsage::CpuRead | BufferUsage::CopyDestination);
    auto list = take(device.create_command_list(QueueType::Copy));
    CHECK(fails_with(list->transition(readback, ResourceState::Common, ResourceState::CopySource), RhiErrorCode::StateMismatch));
    CHECK(fails_with(list->transition(readback, ResourceState::CopyDestination, ResourceState::CopySource),
                     RhiErrorCode::InvalidDescriptor));
    require_ok(list->transition(readback, ResourceState::CopyDestination, ResourceState::CopyDestination));
    require_ok(list->close());
}

TEST_CASE("Texture-to-buffer copies check format, states, placement, and footprint", "[unit]") {
    ValidationDevice device;
    TextureDesc color;
    color.width = 64;
    color.height = 64;
    color.format = Format::R8G8B8A8Unorm;
    color.usage = TextureUsage::CopySource;
    const TextureHandle source = take(device.create_texture(color));
    const TextureHandle untransitioned = take(device.create_texture(color));

    TextureDesc depth = color;
    depth.format = Format::D32Float;
    depth.usage = TextureUsage::DepthStencil | TextureUsage::CopySource;
    const TextureHandle depth_source = take(device.create_texture(depth));

    // 64 rows at the 256-byte pitch: the footprint is exactly 16384 bytes.
    const BufferHandle readback = make_buffer(device, 16384, BufferUsage::CopyDestination);

    auto list = take(device.create_command_list(QueueType::Copy));
    require_ok(list->transition(source, ResourceState::Common, ResourceState::CopySource));
    require_ok(list->transition(readback, ResourceState::Common, ResourceState::CopyDestination));
    CHECK(fails_with(list->copy_texture_to_buffer(source, readback, 256), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(list->copy_texture_to_buffer(source, readback, 512), RhiErrorCode::OutOfRange));
    CHECK(fails_with(list->copy_texture_to_buffer(untransitioned, readback, 0), RhiErrorCode::StateMismatch));
    require_ok(list->transition(depth_source, ResourceState::Common, ResourceState::CopySource));
    CHECK(fails_with(list->copy_texture_to_buffer(depth_source, readback, 0), RhiErrorCode::InvalidDescriptor));
    require_ok(list->copy_texture_to_buffer(source, readback, 0));
    require_ok(list->close());
}

TEST_CASE("The reference device has no presentation surface and keeps no buffer contents", "[unit]") {
    ValidationDevice device;
    const BufferHandle readback = make_buffer(device, 64, BufferUsage::CpuRead | BufferUsage::CopyDestination);
    std::array<std::uint8_t, 64> bytes{};
    CHECK(fails_with(device.read_buffer(readback, 0, 64, bytes.data()), RhiErrorCode::Unsupported));

    SwapchainDesc swapchain;
    swapchain.width = 64;
    swapchain.height = 64;
    CHECK(fails_with(device.create_swapchain(swapchain), RhiErrorCode::Unsupported));
    CHECK(device.check_debug_messages().ok());
}

TEST_CASE("A render pass may clear its color targets when it begins", "[unit]") {
    ValidationDevice device;
    const TextureHandle target = make_color(device);
    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(target, ResourceState::Common, ResourceState::RenderTarget));

    RenderPassDesc pass;
    pass.color_targets = {target};
    pass.clear_color = std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
    require_ok(list->begin_render_pass(pass));
    require_ok(list->end_render_pass());
    require_ok(list->close());
}

TEST_CASE("A CPU-writable buffer must be copy-source, starts there, and allows no other state", "[unit]") {
    ValidationDevice device;
    BufferDesc alone;
    alone.size_bytes = 64;
    alone.usage = BufferUsage::CpuWrite;
    CHECK(fails_with(device.create_buffer(alone), RhiErrorCode::InvalidDescriptor));
    BufferDesc both;
    both.size_bytes = 64;
    both.usage = BufferUsage::CpuWrite | BufferUsage::CopySource | BufferUsage::CpuRead | BufferUsage::CopyDestination;
    CHECK(fails_with(device.create_buffer(both), RhiErrorCode::InvalidDescriptor));

    const BufferHandle upload = make_buffer(device, 64, BufferUsage::CpuWrite | BufferUsage::CopySource);
    auto list = take(device.create_command_list(QueueType::Copy));
    CHECK(fails_with(list->transition(upload, ResourceState::Common, ResourceState::CopyDestination), RhiErrorCode::StateMismatch));
    CHECK(fails_with(list->transition(upload, ResourceState::CopySource, ResourceState::Common), RhiErrorCode::InvalidDescriptor));
    require_ok(list->transition(upload, ResourceState::CopySource, ResourceState::CopySource));
    require_ok(list->close());
}

TEST_CASE("Uploads are checked, the reference device keeps no contents, and a queue wait names a submitted value", "[unit]") {
    ValidationDevice device;
    const BufferHandle upload = make_buffer(device, 64, BufferUsage::CpuWrite | BufferUsage::CopySource);
    const BufferHandle plain = make_buffer(device, 64, BufferUsage::CopyDestination);
    const std::uint32_t word = 7;
    CHECK(fails_with(device.upload_buffer(plain, 0, 4, &word), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(device.upload_buffer(upload, 60, 8, &word), RhiErrorCode::OutOfRange));
    CHECK(fails_with(device.upload_buffer(upload, 0, 4, &word), RhiErrorCode::Unsupported));

    const FenceHandle fence = take(device.create_fence(0));
    CHECK(fails_with(device.queue_wait(QueueType::Compute, fence, 1), RhiErrorCode::InvalidState));
    auto list = take(device.create_command_list(QueueType::Copy));
    require_ok(list->close());
    require_ok(device.submit(QueueType::Copy, {list.get()}, fence, 1));
    require_ok(device.queue_wait(QueueType::Compute, fence, 1));
    CHECK(fails_with(device.queue_wait(QueueType::Compute, fence, 2), RhiErrorCode::InvalidState));
}

TEST_CASE("Resource counts follow live handles, and buffer-to-texture copies check their rules", "[unit]") {
    ValidationDevice device;
    const ResourceStats before = take(device.resource_stats());
    const BufferHandle upload = make_buffer(device, 1024, BufferUsage::CpuWrite | BufferUsage::CopySource);
    const BufferHandle small = make_buffer(device, 64, BufferUsage::CpuWrite | BufferUsage::CopySource);
    const BufferHandle scratch = make_buffer(device, 64, BufferUsage::CopyDestination);
    TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = Format::R8G8B8A8Unorm;
    desc.usage = TextureUsage::CopyDestination;
    const TextureHandle texture = take(device.create_texture(desc));
    const TextureHandle untouched = take(device.create_texture(desc));
    CHECK(take(device.resource_stats()).buffers == before.buffers + 3);
    CHECK(take(device.resource_stats()).textures == before.textures + 2);
    require_ok(device.destroy(scratch));
    CHECK(take(device.resource_stats()).buffers == before.buffers + 2);

    auto list = take(device.create_command_list(QueueType::Copy));
    require_ok(list->transition(texture, ResourceState::Common, ResourceState::CopyDestination));
    // Each row of a 4-texel RGBA8 texture takes 256 bytes, so the footprint is 784 bytes and must start on a 512-byte boundary.
    CHECK(fails_with(list->copy_buffer_to_texture(upload, 16, texture), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(list->copy_buffer_to_texture(small, 0, texture), RhiErrorCode::OutOfRange));
    CHECK(fails_with(list->copy_buffer_to_texture(upload, 0, untouched), RhiErrorCode::StateMismatch));
    require_ok(list->copy_buffer_to_texture(upload, 0, texture));
    require_ok(list->close());
}

TEST_CASE("Vertex input is checked when the pipeline is created", "[unit]") {
    ValidationDevice device;
    PipelineDesc desc;
    desc.kind = PipelineKind::Graphics;
    desc.shaders = {ShaderBinary{ShaderStage::Vertex, "main", {1}}, ShaderBinary{ShaderStage::Pixel, "main", {2}}};
    desc.color_formats = {Format::R8G8B8A8Unorm};
    desc.vertex_stride = 12;
    desc.vertex_attributes = {engine::rhi::VertexAttribute{0, Format::R32G32B32Float, 0}};
    CHECK(device.create_pipeline(desc).ok());

    PipelineDesc unsupported_format = desc;
    unsupported_format.vertex_attributes = {engine::rhi::VertexAttribute{0, Format::R32Float, 0}};
    CHECK(fails_with(device.create_pipeline(unsupported_format), RhiErrorCode::InvalidDescriptor));

    PipelineDesc outside_stride = desc;
    outside_stride.vertex_attributes = {engine::rhi::VertexAttribute{0, Format::R32G32B32Float, 4}};
    CHECK(fails_with(device.create_pipeline(outside_stride), RhiErrorCode::OutOfRange));

    PipelineDesc shared_location = desc;
    shared_location.vertex_attributes = {engine::rhi::VertexAttribute{0, Format::R32G32B32Float, 0},
                                         engine::rhi::VertexAttribute{0, Format::R32G32B32Float, 0}};
    CHECK(fails_with(device.create_pipeline(shared_location), RhiErrorCode::InvalidDescriptor));

    PipelineDesc stride_without_attributes = desc;
    stride_without_attributes.vertex_attributes.clear();
    CHECK(fails_with(device.create_pipeline(stride_without_attributes), RhiErrorCode::InvalidDescriptor));

    PipelineDesc compute_with_input;
    compute_with_input.kind = PipelineKind::Compute;
    compute_with_input.shaders = {ShaderBinary{ShaderStage::Compute, "main", {1}}};
    compute_with_input.vertex_stride = 12;
    compute_with_input.vertex_attributes = {engine::rhi::VertexAttribute{0, Format::R32G32B32Float, 0}};
    CHECK(fails_with(device.create_pipeline(compute_with_input), RhiErrorCode::InvalidDescriptor));
}

TEST_CASE("Per-instance input has its own stride and locations, and a draw needs slot 1 bound", "[unit]") {
    ValidationDevice device;
    PipelineDesc desc;
    desc.kind = PipelineKind::Graphics;
    desc.shaders = {ShaderBinary{ShaderStage::Vertex, "main", {1}}, ShaderBinary{ShaderStage::Pixel, "main", {2}}};
    desc.color_formats = {Format::R8G8B8A8Unorm};
    desc.vertex_stride = 12;
    desc.vertex_attributes = {engine::rhi::VertexAttribute{0, Format::R32G32B32Float, 0}};
    desc.instance_stride = 4;
    desc.instance_attributes = {engine::rhi::VertexAttribute{8, Format::R32Uint, 0}};
    CHECK(device.create_pipeline(desc).ok());

    PipelineDesc no_stride = desc;
    no_stride.instance_stride = 0;
    CHECK(fails_with(device.create_pipeline(no_stride), RhiErrorCode::InvalidDescriptor));

    PipelineDesc stride_without_attributes = desc;
    stride_without_attributes.instance_attributes.clear();
    CHECK(fails_with(device.create_pipeline(stride_without_attributes), RhiErrorCode::InvalidDescriptor));

    PipelineDesc clashing_location = desc;
    clashing_location.instance_attributes = {engine::rhi::VertexAttribute{0, Format::R32Uint, 0}};
    CHECK(fails_with(device.create_pipeline(clashing_location), RhiErrorCode::InvalidDescriptor));

    PipelineDesc outside_stride = desc;
    outside_stride.instance_attributes = {engine::rhi::VertexAttribute{8, Format::R32G32Float, 0}};
    CHECK(fails_with(device.create_pipeline(outside_stride), RhiErrorCode::OutOfRange));

    PipelineDesc without_vertex_input = desc;
    without_vertex_input.vertex_attributes.clear();
    without_vertex_input.vertex_stride = 0;
    CHECK(fails_with(device.create_pipeline(without_vertex_input), RhiErrorCode::InvalidDescriptor));

    // The pipeline reads slot 1, so a draw without it bound is an error; binding it makes the same draw valid.
    const PipelineHandle pipeline = take(device.create_pipeline(desc));
    const BufferHandle vertices = make_buffer(device, 256, BufferUsage::Vertex);
    const BufferHandle instances = make_buffer(device, 64, BufferUsage::Vertex);
    const BufferHandle indices = make_buffer(device, 256, BufferUsage::Index);
    const TextureHandle target = make_color(device);
    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(vertices, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->transition(instances, ResourceState::Common, ResourceState::VertexBuffer));
    require_ok(list->transition(indices, ResourceState::Common, ResourceState::IndexBuffer));
    require_ok(list->transition(target, ResourceState::Common, ResourceState::RenderTarget));
    require_ok(list->set_pipeline(pipeline));
    require_ok(list->set_vertex_buffer(0, vertices));
    require_ok(list->set_index_buffer(indices, IndexFormat::Uint32));
    RenderPassDesc pass;
    pass.color_targets = {target};
    require_ok(list->begin_render_pass(pass));
    CHECK(fails_with(list->draw_indexed(3, 1, 0, 0, 0), RhiErrorCode::InvalidState));
    require_ok(list->set_vertex_buffer(1, instances));
    require_ok(list->draw_indexed(3, 1, 0, 0, 0));
    require_ok(list->end_render_pass());
    require_ok(list->close());
}

TEST_CASE("Alpha blending needs blendable color targets, and a two-component UV attribute is accepted", "[unit]") {
    ValidationDevice device;
    PipelineDesc desc;
    desc.kind = PipelineKind::Graphics;
    desc.shaders = {ShaderBinary{ShaderStage::Vertex, "main", {1}}, ShaderBinary{ShaderStage::Pixel, "main", {2}}};
    desc.color_formats = {Format::R8G8B8A8Unorm};
    desc.vertex_stride = 20;
    desc.vertex_attributes = {engine::rhi::VertexAttribute{0, Format::R32G32B32Float, 0},
                              engine::rhi::VertexAttribute{1, Format::R32G32Float, 12}};
    CHECK(device.create_pipeline(desc).ok());

    // Integer and R32Float targets do not blend on every device, so a blended pipeline may not name them.
    PipelineDesc blend = desc;
    blend.alpha_blend = true;
    CHECK(device.create_pipeline(blend).ok());
    PipelineDesc entity_blend = blend;
    entity_blend.color_formats = {Format::R32Uint};
    CHECK(fails_with(device.create_pipeline(entity_blend), RhiErrorCode::InvalidDescriptor));
    PipelineDesc float_blend = blend;
    float_blend.color_formats = {Format::R32Float};
    CHECK(fails_with(device.create_pipeline(float_blend), RhiErrorCode::InvalidDescriptor));
    PipelineDesc no_targets = blend;
    no_targets.color_formats.clear();
    CHECK(fails_with(device.create_pipeline(no_targets), RhiErrorCode::InvalidDescriptor));

    // Additive blending (US-051) follows the same rules, and it excludes alpha blending.
    PipelineDesc additive = desc;
    additive.additive_blend = true;
    CHECK(device.create_pipeline(additive).ok());
    PipelineDesc additive_both = additive;
    additive_both.alpha_blend = true;
    CHECK(fails_with(device.create_pipeline(additive_both), RhiErrorCode::InvalidDescriptor));
    PipelineDesc additive_entity = additive;
    additive_entity.color_formats = {Format::R32Uint};
    CHECK(fails_with(device.create_pipeline(additive_entity), RhiErrorCode::InvalidDescriptor));

    // Cull mode and depth writes are per pipeline and need no checks of their own.
    PipelineDesc two_sided = desc;
    two_sided.cull_mode = engine::rhi::CullMode::None;
    two_sided.depth_format = Format::D32Float;
    two_sided.depth_write = false;
    CHECK(device.create_pipeline(two_sided).ok());

    PipelineDesc uv_as_float3 = desc;
    uv_as_float3.vertex_attributes = {engine::rhi::VertexAttribute{1, Format::R32Float, 12}};
    CHECK(fails_with(device.create_pipeline(uv_as_float3), RhiErrorCode::InvalidDescriptor));
}

TEST_CASE("A depth clear needs a depth target", "[unit]") {
    ValidationDevice device;
    const TextureHandle color = make_color(device);
    auto list = take(device.create_command_list(QueueType::Graphics));
    require_ok(list->transition(color, ResourceState::Common, ResourceState::RenderTarget));
    RenderPassDesc pass;
    pass.color_targets = {color};
    pass.clear_depth = 1.0f;
    CHECK(fails_with(list->begin_render_pass(pass), RhiErrorCode::InvalidDescriptor));
}

TEST_CASE("The reference device reports ray tracing as FeatureDisabled, or Unsupported when simulated", "[unit]") {
    // Without simulated ray tracing hardware, or with the feature forced off, the commands fail with FeatureDisabled.
    ValidationDeviceDesc no_hardware;
    no_hardware.simulated_hardware = Feature::MeshShaders;
    ValidationDevice absent(no_hardware);
    CHECK(fails_with(absent.create_bottom_level(BottomLevelDesc{}), RhiErrorCode::FeatureDisabled));
    CHECK(fails_with(absent.create_top_level(TopLevelDesc{}), RhiErrorCode::FeatureDisabled));
    CHECK(fails_with(absent.create_ray_tracing_pipeline(RayTracingPipelineDesc{}), RhiErrorCode::FeatureDisabled));

    ValidationDeviceDesc forced_off;
    forced_off.disabled_features = Feature::RayTracing;
    ValidationDevice disabled(forced_off);
    CHECK(disabled.capabilities().state(Feature::RayTracing) == FeatureState::DisabledForTest);
    CHECK(fails_with(disabled.create_bottom_level(BottomLevelDesc{}), RhiErrorCode::FeatureDisabled));

    // A simulated feature is enabled in the report, but the reference device keeps no acceleration structures, so the commands
    // that would use them are Unsupported rather than silently accepted.
    ValidationDevice simulated;
    CHECK(simulated.capabilities().enabled(Feature::RayTracing));
    CHECK(fails_with(simulated.create_bottom_level(BottomLevelDesc{}), RhiErrorCode::Unsupported));
    CHECK(fails_with(simulated.create_top_level(TopLevelDesc{}), RhiErrorCode::Unsupported));
    CHECK(fails_with(simulated.create_ray_tracing_pipeline(RayTracingPipelineDesc{}), RhiErrorCode::Unsupported));
    CHECK(fails_with(simulated.destroy(AccelerationStructureHandle{}), RhiErrorCode::Unsupported));

    auto list = simulated.create_command_list(QueueType::Graphics);
    REQUIRE(list.ok());
    CHECK(fails_with(list.value()->build_bottom_level(AccelerationStructureHandle{}, BuildMode::Build), RhiErrorCode::Unsupported));
    CHECK(fails_with(list.value()->set_ray_tracing_pipeline(RayTracingPipelineHandle{}), RhiErrorCode::Unsupported));
    CHECK(fails_with(list.value()->dispatch_rays(1, 1, 1), RhiErrorCode::Unsupported));
}
