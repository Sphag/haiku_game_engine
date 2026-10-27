// GPU tests for the DX12 backend (US-027). Tagged [gpu][dx12], so only `engine_tests --suite gpu --backend dx12` runs them, on a
// Windows machine with a hardware adapter and a desktop session. Each test creates its device with the D3D12 debug layer and GPU-based
// validation on; any error the layer reports fails the test. Nothing here runs on a software adapter.

#include "engine/core/result.hpp"
#include "engine/platform/platform.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/swapchain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using engine::platform::Platform;
using engine::platform::WindowDesc;
using engine::platform::WindowId;
using engine::rhi::BufferDesc;
using engine::rhi::BufferHandle;
using engine::rhi::BufferUsage;
using engine::rhi::Backend;
using engine::rhi::CommandList;
using engine::rhi::Device;
using engine::rhi::DescriptorBindingDesc;
using engine::rhi::DescriptorSetHandle;
using engine::rhi::DescriptorSetLayoutDesc;
using engine::rhi::DescriptorSetLayoutHandle;
using engine::rhi::DescriptorType;
using engine::rhi::DeviceDesc;
using engine::rhi::Feature;
using engine::rhi::FeatureState;
using engine::rhi::FenceHandle;
using engine::rhi::Format;
using engine::rhi::PipelineDesc;
using engine::rhi::PipelineHandle;
using engine::rhi::PipelineKind;
using engine::rhi::PresentSettings;
using engine::rhi::QueryPoolHandle;
using engine::rhi::QueueType;
using engine::rhi::RenderPassDesc;
using engine::rhi::ResourceState;
using engine::rhi::ResourceStats;
using engine::rhi::ShaderBinary;
using engine::rhi::ShaderStage;
using engine::rhi::RhiErrorCode;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::Swapchain;
using engine::rhi::SwapchainDesc;
using engine::rhi::TextureDesc;
using engine::rhi::TextureHandle;
using engine::rhi::TextureUsage;
using engine::rhi::AccelerationStructureHandle;
using engine::rhi::BottomLevelDesc;
using engine::rhi::BuildMode;
using engine::rhi::IndexFormat;
using engine::rhi::RayTracingPipelineDesc;
using engine::rhi::RayTracingPipelineHandle;
using engine::rhi::TopLevelDesc;
using engine::rhi::TopLevelInstance;

namespace {

// The reference clear color. Each channel converts to an 8-bit unorm value by rounding value * 255.
constexpr std::array<float, 4> kReferenceClear{0.25f, 0.5f, 0.75f, 1.0f};

std::uint8_t expected_channel(float value) {
    return static_cast<std::uint8_t>(std::lround(value * 255.0f));
}

// The channel value a readback may differ by: one unit covers the rounding of the float-to-unorm conversion.
bool near_channel(std::uint8_t actual, float expected) {
    const int difference = static_cast<int>(actual) - static_cast<int>(expected_channel(expected));
    return difference >= -1 && difference <= 1;
}

template <typename T>
T take(RhiResult<T> result) {
    if (!result.ok()) {
        FAIL(result.error().message);
    }
    return std::move(result).value();
}

template <typename T>
T must(engine::core::Result<T, engine::platform::PlatformError> result) {
    if (!result.ok()) {
        FAIL(result.error().detail);
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

std::string_view feature_name(Feature feature) {
    switch (feature) {
        case Feature::MeshShaders:
            return "MeshShaders";
        case Feature::IndirectCount:
            return "IndirectCount";
        case Feature::RayTracing:
            return "RayTracing";
        case Feature::TimestampQueries:
            return "TimestampQueries";
        case Feature::None:
            break;
    }
    return "None";
}

DeviceDesc debug_dx12() {
    DeviceDesc desc;
    desc.backend = Backend::Dx12;
    desc.debug_layers = true;
    return desc;
}

// Owns a platform, one window, a device, and a swapchain. Teardown order matters: the swapchain holds the window's surface, the device
// owns the back buffers, and the platform owns the window.
struct Harness {
    std::unique_ptr<Platform> platform;
    WindowId window;
    std::unique_ptr<Device> device;
    std::unique_ptr<Swapchain> swapchain;

    ~Harness() {
        swapchain.reset();
        device.reset();
        platform.reset();
    }
};

void open_window(Harness& harness) {
    harness.platform = must(Platform::create());
    WindowDesc window;
    window.title = "US-027 DX12 smoke test";
    window.size = {256.0, 256.0};
    harness.window = must(harness.platform->create_window(window));
}

SwapchainDesc swapchain_for(Harness& harness, PresentSettings present) {
    const auto info = must(harness.platform->window_info(harness.window));
    const auto handles = must(harness.platform->win32_handles(harness.window));
    SwapchainDesc desc;
    desc.hwnd = handles.hwnd;
    desc.width = info.framebuffer_size.width;
    desc.height = info.framebuffer_size.height;
    desc.format = Format::B8G8R8A8Unorm;
    desc.buffer_count = 2;
    desc.present = present;
    return desc;
}

void open_harness(Harness& harness, PresentSettings present = {}) {
    open_window(harness);
    harness.device = take(engine::rhi::create_device(debug_dx12()));
    harness.swapchain = take(harness.device->create_swapchain(swapchain_for(harness, present)));
}

void require_no_debug_errors(Device& device) {
    const RhiStatus status = device.check_debug_messages();
    if (!status.ok()) {
        FAIL(status.error().message);
    }
}

}  // namespace

TEST_CASE("DX12 reports its backend, adapter, and capabilities", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    const auto& report = device->capabilities();

    CHECK(device->backend() == Backend::Dx12);
    CHECK(report.backend == Backend::Dx12);
    CHECK_FALSE(report.adapter_name.empty());
    CHECK(report.validation_layers);
    CHECK(report.timestamp_frequency_hz > 0);
    // Indirect count is core D3D12. Mesh shaders and ray tracing depend on the adapter's tiers, so they are reported, not assumed.
    CHECK(report.state(Feature::IndirectCount) == FeatureState::Supported);
    CHECK(report.state(Feature::TimestampQueries) == FeatureState::Supported);

    // The per-feature report is printed so that a run leaves a record of what the adapter supports.
    std::printf("[gpu-report] backend=dx12 adapter=\"%s\" validation_layers=on timestamp_hz=%llu\n", report.adapter_name.c_str(),
                static_cast<unsigned long long>(report.timestamp_frequency_hz));
    for (Feature feature : engine::rhi::kFeatureList) {
        const std::string line = std::string(feature_name(feature)) + " = " + std::string(engine::rhi::to_string(report.state(feature)));
        std::printf("[gpu-report]   %s\n", line.c_str());
    }

    DeviceDesc forced = debug_dx12();
    forced.disabled_features = Feature::TimestampQueries;
    auto without_timestamps = take(engine::rhi::create_device(forced));
    CHECK(without_timestamps->capabilities().state(Feature::TimestampQueries) == FeatureState::DisabledForTest);
}

TEST_CASE("DX12 rejects draws without a pass, a pipeline, or input buffers, and an empty layout", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    // An empty layout is invalid, not unsupported: descriptor layouts exist on DX12 since US-029.
    CHECK(fails_with(device->create_descriptor_set_layout(engine::rhi::DescriptorSetLayoutDesc{}), RhiErrorCode::InvalidDescriptor));
    auto list = take(device->create_command_list(QueueType::Graphics));
    // Graphics pipelines exist since US-033; a draw still needs an active pass and bound inputs.
    CHECK(fails_with(list->draw_indexed(3, 1, 0, 0, 0), RhiErrorCode::InvalidState));
    CHECK(fails_with(list->set_vertex_buffer(2, BufferHandle{}), RhiErrorCode::OutOfRange));
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 clears an offscreen target and reads back the reference color", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));

    TextureDesc target_desc;
    target_desc.width = 64;
    target_desc.height = 64;
    target_desc.format = Format::R8G8B8A8Unorm;
    target_desc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    target_desc.debug_name = "reference clear target";
    const TextureHandle target = take(device->create_texture(target_desc));

    const std::uint64_t pitch = engine::rhi::copy_row_pitch(target_desc.width, target_desc.format);
    BufferDesc readback_desc;
    readback_desc.size_bytes = pitch * target_desc.height;
    readback_desc.usage = BufferUsage::CpuRead | BufferUsage::CopyDestination;
    readback_desc.debug_name = "reference clear readback";
    const BufferHandle readback = take(device->create_buffer(readback_desc));

    auto list = take(device->create_command_list(QueueType::Graphics));
    require_ok(list->transition(target, ResourceState::Common, ResourceState::RenderTarget));
    RenderPassDesc pass;
    pass.color_targets = {target};
    pass.clear_color = kReferenceClear;
    require_ok(list->begin_render_pass(pass));
    require_ok(list->end_render_pass());
    require_ok(list->transition(target, ResourceState::RenderTarget, ResourceState::CopySource));
    require_ok(list->copy_texture_to_buffer(target, readback, 0));
    require_ok(list->close());

    const FenceHandle fence = take(device->create_fence(0));
    std::vector<CommandList*> lists{list.get()};
    require_ok(device->submit(QueueType::Graphics, lists, fence, 1));
    require_ok(device->wait_fence(fence, 1));
    CHECK(take(device->fence_value(fence)) == 1);

    std::vector<std::uint8_t> bytes(readback_desc.size_bytes);
    require_ok(device->read_buffer(readback, 0, bytes.size(), bytes.data()));
    for (std::uint32_t y : {0u, 31u, 63u}) {
        for (std::uint32_t x : {0u, 31u, 63u}) {
            const std::size_t offset = y * pitch + x * 4;
            INFO("texel " << x << "," << y);
            CHECK(near_channel(bytes[offset + 0], kReferenceClear[0]));
            CHECK(near_channel(bytes[offset + 1], kReferenceClear[1]));
            CHECK(near_channel(bytes[offset + 2], kReferenceClear[2]));
            CHECK(near_channel(bytes[offset + 3], kReferenceClear[3]));
        }
    }
    require_ok(device->destroy(readback));
    require_ok(device->destroy(target));
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 presents frames, and the back buffer reads back the reference color", "[gpu][dx12]") {
    Harness harness;
    open_harness(harness);
    Device& device = *harness.device;
    Swapchain& swapchain = *harness.swapchain;

    const std::uint32_t width = swapchain.width();
    const std::uint32_t height = swapchain.height();
    REQUIRE(width > 0);
    REQUIRE(height > 0);
    const std::uint64_t pitch = engine::rhi::copy_row_pitch(width, Format::B8G8R8A8Unorm);
    BufferDesc readback_desc;
    readback_desc.size_bytes = pitch * height;
    readback_desc.usage = BufferUsage::CpuRead | BufferUsage::CopyDestination;
    const BufferHandle readback = take(device.create_buffer(readback_desc));
    const FenceHandle fence = take(device.create_fence(0));

    constexpr int kFrames = 3;
    for (std::uint64_t frame = 1; frame <= kFrames; ++frame) {
        const auto back = take(swapchain.acquire_back_buffer());
        REQUIRE(back.has_value());
        auto list = take(device.create_command_list(QueueType::Graphics));
        require_ok(list->transition(*back, ResourceState::Present, ResourceState::RenderTarget));
        RenderPassDesc pass;
        pass.color_targets = {*back};
        pass.clear_color = kReferenceClear;
        require_ok(list->begin_render_pass(pass));
        require_ok(list->end_render_pass());
        require_ok(list->transition(*back, ResourceState::RenderTarget, ResourceState::CopySource));
        if (frame == kFrames) {
            require_ok(list->copy_texture_to_buffer(*back, readback, 0));
        }
        require_ok(list->transition(*back, ResourceState::CopySource, ResourceState::Present));
        require_ok(list->close());
        std::vector<CommandList*> lists{list.get()};
        require_ok(device.submit(QueueType::Graphics, lists, fence, frame));
        require_ok(swapchain.present());
    }
    require_ok(device.wait_idle());

    // The swapchain is B8G8R8A8, so each texel is stored blue first.
    std::vector<std::uint8_t> bytes(readback_desc.size_bytes);
    require_ok(device.read_buffer(readback, 0, bytes.size(), bytes.data()));
    const std::size_t offset = (height / 2) * pitch + (width / 2) * 4;
    CHECK(near_channel(bytes[offset + 2], kReferenceClear[0]));
    CHECK(near_channel(bytes[offset + 1], kReferenceClear[1]));
    CHECK(near_channel(bytes[offset + 0], kReferenceClear[2]));
    CHECK(near_channel(bytes[offset + 3], kReferenceClear[3]));

    require_ok(device.destroy(readback));
    require_ok(device.destroy(fence));
    require_no_debug_errors(device);
}

TEST_CASE("DX12 rejects a present whose back buffer is not in the Present state", "[gpu][dx12]") {
    Harness harness;
    open_harness(harness);
    Device& device = *harness.device;
    Swapchain& swapchain = *harness.swapchain;
    const FenceHandle fence = take(device.create_fence(0));

    const auto back = take(swapchain.acquire_back_buffer());
    REQUIRE(back.has_value());
    // Move the buffer out of Present and submit that, so the tracked state matches the GPU.
    auto leave = take(device.create_command_list(QueueType::Graphics));
    require_ok(leave->transition(*back, ResourceState::Present, ResourceState::RenderTarget));
    require_ok(leave->close());
    std::vector<CommandList*> first{leave.get()};
    require_ok(device.submit(QueueType::Graphics, first, fence, 1));
    CHECK(fails_with(swapchain.present(), RhiErrorCode::StateMismatch));

    // The rejected present left the buffer acquired. Return it to Present, then present it for real.
    auto restore = take(device.create_command_list(QueueType::Graphics));
    require_ok(restore->transition(*back, ResourceState::RenderTarget, ResourceState::Present));
    require_ok(restore->close());
    std::vector<CommandList*> second{restore.get()};
    require_ok(device.submit(QueueType::Graphics, second, fence, 2));
    require_ok(swapchain.present());
    require_ok(device.wait_idle());
    require_no_debug_errors(device);
}

TEST_CASE("DX12 resize, minimize, and restore keep presenting, and teardown waits for queued work", "[gpu][dx12]") {
    Harness harness;
    open_harness(harness);
    Device& device = *harness.device;
    Swapchain& swapchain = *harness.swapchain;
    const FenceHandle fence = take(device.create_fence(0));
    std::uint64_t value = 0;

    auto present_once = [&]() {
        const auto back = take(swapchain.acquire_back_buffer());
        REQUIRE(back.has_value());
        auto list = take(device.create_command_list(QueueType::Graphics));
        require_ok(list->transition(*back, ResourceState::Present, ResourceState::RenderTarget));
        RenderPassDesc pass;
        pass.color_targets = {*back};
        pass.clear_color = kReferenceClear;
        require_ok(list->begin_render_pass(pass));
        require_ok(list->end_render_pass());
        require_ok(list->transition(*back, ResourceState::RenderTarget, ResourceState::Present));
        require_ok(list->close());
        std::vector<CommandList*> lists{list.get()};
        require_ok(device.submit(QueueType::Graphics, lists, fence, ++value));
        require_ok(swapchain.present());
    };

    present_once();
    require_ok(swapchain.resize(192, 128));
    CHECK(swapchain.width() == 192);
    present_once();

    // Minimized: the framebuffer is empty, so no buffer is handed out and the frame is skipped.
    require_ok(swapchain.resize(0, 0));
    CHECK(take(swapchain.acquire_back_buffer()) == std::nullopt);

    require_ok(swapchain.resize(320, 200));
    present_once();
    // Resizing to the size already in place changes nothing.
    require_ok(swapchain.resize(320, 200));
    present_once();

    // Queued but not waited-on work must finish before teardown, which the harness destructor does.
    require_ok(device.wait_fence(fence, value));
    require_no_debug_errors(device);
}

TEST_CASE("DX12 presentation settings report the effective policy", "[gpu][dx12]") {
    Harness vsync_on;
    open_harness(vsync_on, PresentSettings{true, true});
    CHECK(vsync_on.swapchain->present_settings().vsync);
    CHECK_FALSE(vsync_on.swapchain->present_settings().allow_tearing);
    vsync_on.swapchain.reset();

    // Tearing needs adapter support. Without it, vsync stays on; with it, the request holds. Either way the effective policy says which.
    Harness vsync_off;
    open_harness(vsync_off, PresentSettings{false, true});
    const PresentSettings effective = vsync_off.swapchain->present_settings();
    CHECK(effective.vsync != effective.allow_tearing);
    require_no_debug_errors(*vsync_off.device);
}

TEST_CASE("DX12 fences order submissions, and timestamps resolve only after their submission completes", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    const QueryPoolHandle pool = take(device->create_query_pool(2));
    const FenceHandle fence = take(device->create_fence(0));

    auto list = take(device->create_command_list(QueueType::Graphics));
    require_ok(list->write_timestamp(pool, 0));
    require_ok(list->close());
    CHECK(fails_with(device->read_timestamp(pool, 0), RhiErrorCode::NotReady));

    std::vector<CommandList*> lists{list.get()};
    require_ok(device->submit(QueueType::Graphics, lists, fence, 1));
    // A value no submission will reach cannot be waited on.
    CHECK(fails_with(device->wait_fence(fence, 2), RhiErrorCode::InvalidState));
    require_ok(device->wait_fence(fence, 1));
    CHECK(take(device->read_timestamp(pool, 0)) > 0);
    CHECK(take(device->fence_value(fence)) == 1);
    require_no_debug_errors(*device);
}

// ---- US-029: resources and descriptors ----

namespace {

constexpr std::uint32_t kElements = 256;
constexpr std::uint64_t kElementBytes = kElements * sizeof(float);
constexpr std::uint64_t kParamBytes = 16;

// The compute fixture reads Params (constant buffer, binding 0), Input (binding 1), and Output (binding 2). Keep this struct in step
// with dx12_compute.hlsl.
struct ComputeParams {
    float scale = 1.0f;
    float offset = 0.0f;
    std::uint32_t count = 0;
    std::uint32_t pad = 0;
};

std::vector<std::uint8_t> load_shader(std::string_view name) {
    const std::string path = std::string(ENGINE_TEST_SHADER_DIR) + "/" + std::string(name) + ".dxil";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        FAIL("missing shader " + path + "; build the engine_shader_" + std::string(name) + " target");
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

BufferHandle make_buffer(Device& device, std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return take(device.create_buffer(desc));
}

TextureHandle make_texture(Device& device, std::uint32_t width, std::uint32_t height, TextureUsage usage, const char* name) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = Format::R8G8B8A8Unorm;
    desc.usage = usage;
    desc.debug_name = name;
    return take(device.create_texture(desc));
}

RhiStatus submit_one(Device& device, QueueType queue, CommandList& list, FenceHandle fence, std::uint64_t value) {
    const std::vector<CommandList*> lists{&list};
    return device.submit(queue, lists, fence, value);
}

// One set layout (CBV 0, SRV 1, UAV 2) and the compute pipeline that reads it.
struct ComputeSetup {
    DescriptorSetLayoutHandle layout;
    PipelineHandle pipeline;
};

ComputeSetup make_compute(Device& device) {
    DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings = {
        DescriptorBindingDesc{0, DescriptorType::ConstantBuffer, 1, ShaderStage::Compute},
        DescriptorBindingDesc{1, DescriptorType::ShaderResource, 1, ShaderStage::Compute},
        DescriptorBindingDesc{2, DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
    };
    ComputeSetup setup;
    setup.layout = take(device.create_descriptor_set_layout(layout_desc));

    PipelineDesc pipeline;
    pipeline.kind = PipelineKind::Compute;
    pipeline.shaders.push_back(ShaderBinary{ShaderStage::Compute, "main", load_shader("dx12_compute")});
    pipeline.set_layouts = {setup.layout};
    pipeline.debug_name = "US-029 compute fixture";
    setup.pipeline = take(device.create_pipeline(pipeline));
    return setup;
}

// Records output = input * scale + offset over every element. The constant buffer is read on the compute queue, so the list
// transitions it there. Output is handed back in Common so a copy queue can read it.
std::unique_ptr<CommandList> record_dispatch(Device& device, const ComputeSetup& compute, DescriptorSetHandle set, BufferHandle constants,
                                             BufferHandle input, BufferHandle output) {
    auto list = take(device.create_command_list(QueueType::Compute));
    require_ok(list->transition(constants, ResourceState::Common, ResourceState::ConstantBuffer));
    require_ok(list->transition(input, ResourceState::Common, ResourceState::ShaderRead));
    require_ok(list->transition(output, ResourceState::Common, ResourceState::UnorderedAccess));
    require_ok(list->set_pipeline(compute.pipeline));
    require_ok(list->set_descriptor_set(0, set));
    require_ok(list->dispatch(kElements / 64, 1, 1));
    require_ok(list->transition(output, ResourceState::UnorderedAccess, ResourceState::Common));
    require_ok(list->close());
    return list;
}

}  // namespace

TEST_CASE("DX12 checks descriptor writes, layouts, and resource states before a dispatch", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    const ComputeSetup compute = make_compute(*device);
    const BufferHandle constants = make_buffer(*device, kParamBytes, BufferUsage::CopyDestination | BufferUsage::Constant, "constants");
    const BufferHandle input = make_buffer(*device, kElementBytes, BufferUsage::CopyDestination | BufferUsage::ShaderResource, "input");
    const BufferHandle output = make_buffer(*device, kElementBytes, BufferUsage::UnorderedAccess | BufferUsage::CopySource, "output");

    // A write must match the binding's type and array bounds.
    auto set = take(device->create_descriptor_set(compute.layout));
    CHECK(fails_with(device->write_buffer(set, 1, 0, constants), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(device->write_buffer(set, 2, 1, output), RhiErrorCode::OutOfRange));
    require_ok(device->write_buffer(set, 0, 0, constants));
    require_ok(device->write_buffer(set, 1, 0, input));
    require_ok(device->write_buffer(set, 2, 0, output));

    auto list = take(device->create_command_list(QueueType::Compute));
    require_ok(list->set_pipeline(compute.pipeline));
    // A set must match the pipeline's layout at its index.
    DescriptorSetLayoutDesc other;
    other.bindings = {DescriptorBindingDesc{0, DescriptorType::ShaderResource, 1, ShaderStage::Compute}};
    const auto other_layout = take(device->create_descriptor_set_layout(other));
    const auto other_set = take(device->create_descriptor_set(other_layout));
    CHECK(fails_with(list->set_descriptor_set(0, other_set), RhiErrorCode::InvalidDescriptor));

    // A bound set with a descriptor that was never written cannot dispatch.
    const auto blank = take(device->create_descriptor_set(compute.layout));
    require_ok(list->set_descriptor_set(0, blank));
    CHECK(fails_with(list->dispatch(kElements / 64, 1, 1), RhiErrorCode::InvalidState));

    // The written set is bound, but its resources are in the wrong states until the transitions run.
    require_ok(list->set_descriptor_set(0, set));
    CHECK(fails_with(list->dispatch(kElements / 64, 1, 1), RhiErrorCode::StateMismatch));
    require_ok(list->transition(constants, ResourceState::Common, ResourceState::ConstantBuffer));
    require_ok(list->transition(input, ResourceState::Common, ResourceState::ShaderRead));
    require_ok(list->transition(output, ResourceState::Common, ResourceState::UnorderedAccess));
    CHECK(fails_with(list->dispatch(0, 1, 1), RhiErrorCode::OutOfRange));
    require_ok(list->dispatch(kElements / 64, 1, 1));
    require_ok(list->close());

    // A destroyed resource stays invalid for a list that has not submitted.
    CHECK(device->destroy(input).ok());
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 round-trips buffers through the copy, compute, and copy queues", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    const ComputeSetup compute = make_compute(*device);
    const std::uint64_t input_bytes = kElementBytes;
    const BufferHandle upload_input = make_buffer(*device, input_bytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "upload input");
    const BufferHandle upload_params = make_buffer(*device, kParamBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "upload params");
    const BufferHandle input = make_buffer(*device, input_bytes, BufferUsage::CopyDestination | BufferUsage::ShaderResource, "input");
    const BufferHandle constants = make_buffer(*device, kParamBytes, BufferUsage::CopyDestination | BufferUsage::Constant, "constants");
    const BufferHandle output = make_buffer(*device, input_bytes, BufferUsage::UnorderedAccess | BufferUsage::CopySource, "output");
    const BufferHandle readback = make_buffer(*device, input_bytes, BufferUsage::CopyDestination | BufferUsage::CpuRead, "readback");
    const FenceHandle fence = take(device->create_fence(0));

    // Upload is CPU-writable only, so the device rejects an upload to a buffer that was not created for it.
    CHECK(fails_with(device->upload_buffer(input, 0, 4, nullptr), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(device->upload_buffer(upload_input, input_bytes, 4, nullptr), RhiErrorCode::OutOfRange));

    std::vector<float> values(kElements);
    for (std::uint32_t i = 0; i < kElements; ++i) {
        values[i] = static_cast<float>(i) * 0.5f;
    }
    const ComputeParams params{2.0f, 1.0f, kElements, 0};
    require_ok(device->upload_buffer(upload_input, 0, input_bytes, values.data()));
    require_ok(device->upload_buffer(upload_params, 0, sizeof(params), &params));

    // Copy queue: upload the input and the parameters, then hand both back in Common.
    auto copy_in = take(device->create_command_list(QueueType::Copy));
    require_ok(copy_in->transition(input, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(copy_in->copy_buffer(upload_input, 0, input, 0, input_bytes));
    require_ok(copy_in->transition(input, ResourceState::CopyDestination, ResourceState::Common));
    require_ok(copy_in->transition(constants, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(copy_in->copy_buffer(upload_params, 0, constants, 0, sizeof(params)));
    require_ok(copy_in->transition(constants, ResourceState::CopyDestination, ResourceState::Common));
    require_ok(copy_in->close());
    require_ok(submit_one(*device, QueueType::Copy, *copy_in, fence, 1));

    // Compute queue waits on the GPU for the upload, then dispatches.
    require_ok(device->queue_wait(QueueType::Compute, fence, 1));
    auto set = take(device->create_descriptor_set(compute.layout));
    require_ok(device->write_buffer(set, 0, 0, constants));
    require_ok(device->write_buffer(set, 1, 0, input));
    require_ok(device->write_buffer(set, 2, 0, output));
    auto dispatch = record_dispatch(*device, compute, set, constants, input, output);
    require_ok(submit_one(*device, QueueType::Compute, *dispatch, fence, 2));

    // Copy queue waits for the dispatch, then reads the output back.
    require_ok(device->queue_wait(QueueType::Copy, fence, 2));
    auto copy_out = take(device->create_command_list(QueueType::Copy));
    require_ok(copy_out->transition(output, ResourceState::Common, ResourceState::CopySource));
    require_ok(copy_out->copy_buffer(output, 0, readback, 0, input_bytes));
    require_ok(copy_out->close());
    require_ok(submit_one(*device, QueueType::Copy, *copy_out, fence, 3));

    require_ok(device->wait_fence(fence, 3));
    std::vector<float> result(kElements, -1.0f);
    require_ok(device->read_buffer(readback, 0, input_bytes, result.data()));
    for (std::uint32_t i = 0; i < kElements; ++i) {
        CHECK(result[i] == values[i] * params.scale + params.offset);
    }
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 round-trips texel data through the graphics queue", "[gpu][dx12]") {
    // Texture copies on the copy queue are not yet accepted by the debug layer (see docs/dx12.md), so this round trip runs on the
    // graphics queue, which supports every copy the texture needs.
    auto device = take(engine::rhi::create_device(debug_dx12()));
    constexpr std::uint32_t kSize = 4;
    constexpr std::uint64_t kUploadBytes = 1024;  // row pitch 256 for four 4-byte texels, so rows start at 0, 256, 512, 768
    const BufferHandle upload = make_buffer(*device, kUploadBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "texel upload");
    const BufferHandle readback = make_buffer(*device, kUploadBytes, BufferUsage::CopyDestination | BufferUsage::CpuRead, "texel readback");
    const TextureHandle texture = make_texture(*device, kSize, kSize, TextureUsage::CopyDestination | TextureUsage::CopySource, "texels");
    const FenceHandle fence = take(device->create_fence(0));

    std::vector<std::uint8_t> texels(kUploadBytes, 0);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            std::uint8_t* pixel = texels.data() + y * 256 + x * 4;
            pixel[0] = static_cast<std::uint8_t>(x * 40);
            pixel[1] = static_cast<std::uint8_t>(y * 60);
            pixel[2] = 128;
            pixel[3] = 255;
        }
    }
    require_ok(device->upload_buffer(upload, 0, kUploadBytes, texels.data()));

    auto graphics = take(device->create_command_list(QueueType::Graphics));
    require_ok(graphics->transition(texture, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(graphics->copy_buffer_to_texture(upload, 0, texture));
    require_ok(graphics->transition(texture, ResourceState::CopyDestination, ResourceState::CopySource));
    require_ok(graphics->copy_texture_to_buffer(texture, readback, 0));
    require_ok(graphics->close());
    require_ok(submit_one(*device, QueueType::Graphics, *graphics, fence, 1));

    require_ok(device->wait_fence(fence, 1));
    std::vector<std::uint8_t> result(kUploadBytes, 0);
    require_ok(device->read_buffer(readback, 0, kUploadBytes, result.data()));
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::uint8_t* pixel = result.data() + y * 256 + x * 4;
            CHECK(pixel[0] == static_cast<std::uint8_t>(x * 40));
            CHECK(pixel[1] == static_cast<std::uint8_t>(y * 60));
            CHECK(pixel[2] == 128);
            CHECK(pixel[3] == 255);
        }
    }
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 reuses descriptor sets, buffers, and textures without growing live resources", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    const ComputeSetup compute = make_compute(*device);
    DescriptorSetLayoutDesc texture_layout_desc;
    texture_layout_desc.bindings = {DescriptorBindingDesc{0, DescriptorType::ShaderResource, 1, ShaderStage::Compute}};
    const auto texture_layout = take(device->create_descriptor_set_layout(texture_layout_desc));
    const FenceHandle fence = take(device->create_fence(0));

    const ResourceStats before = take(device->resource_stats());
    constexpr std::uint64_t kRoundTripBytes = 64;
    std::uint64_t fence_value = 0;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const BufferHandle constants = make_buffer(*device, kParamBytes, BufferUsage::CopyDestination | BufferUsage::Constant, "loop constants");
        const BufferHandle input = make_buffer(*device, kElementBytes, BufferUsage::CopyDestination | BufferUsage::ShaderResource, "loop input");
        const BufferHandle output = make_buffer(*device, kElementBytes, BufferUsage::UnorderedAccess | BufferUsage::CopySource, "loop output");
        const BufferHandle upload = make_buffer(*device, kRoundTripBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "loop upload");
        const BufferHandle readback = make_buffer(*device, kRoundTripBytes, BufferUsage::CopyDestination | BufferUsage::CpuRead, "loop readback");
        const TextureHandle texture = make_texture(*device, 4, 4, TextureUsage::ShaderResource, "loop texture");

        auto set = take(device->create_descriptor_set(compute.layout));
        require_ok(device->write_buffer(set, 0, 0, constants));
        require_ok(device->write_buffer(set, 1, 0, input));
        require_ok(device->write_buffer(set, 2, 0, output));
        auto texture_set = take(device->create_descriptor_set(texture_layout));
        require_ok(device->write_texture(texture_set, 0, 0, texture));

        // A GPU round trip per iteration: the copy queue fills the upload and copies it into the readback.
        const std::uint32_t marker = static_cast<std::uint32_t>(iteration + 1);
        require_ok(device->upload_buffer(upload, 0, sizeof(marker), &marker));
        auto copy = take(device->create_command_list(QueueType::Copy));
        require_ok(copy->copy_buffer(upload, 0, readback, 0, kRoundTripBytes));
        require_ok(copy->close());
        fence_value += 1;
        require_ok(submit_one(*device, QueueType::Copy, *copy, fence, fence_value));
        require_ok(device->wait_fence(fence, fence_value));
        std::uint32_t echoed = 0;
        require_ok(device->read_buffer(readback, 0, sizeof(echoed), &echoed));
        CHECK(echoed == marker);

        require_ok(device->destroy(texture_set));
        require_ok(device->destroy(set));
        require_ok(device->destroy(texture));
        require_ok(device->destroy(upload));
        require_ok(device->destroy(readback));
        require_ok(device->destroy(constants));
        require_ok(device->destroy(input));
        require_ok(device->destroy(output));
    }
    require_ok(device->wait_idle());

    const ResourceStats after = take(device->resource_stats());
    CHECK(after.buffers == before.buffers);
    CHECK(after.textures == before.textures);
    CHECK(after.descriptor_sets == before.descriptor_sets);
    CHECK(after.shader_descriptors == before.shader_descriptors);
    CHECK(after.retained_holds == 0);
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 keeps a destroyed set and its buffers for in-flight work, then frees them at the fence", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    const ComputeSetup compute = make_compute(*device);
    const BufferHandle upload_input = make_buffer(*device, kElementBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "held upload");
    const BufferHandle upload_params = make_buffer(*device, kParamBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "held params");
    const BufferHandle input = make_buffer(*device, kElementBytes, BufferUsage::CopyDestination | BufferUsage::ShaderResource, "held input");
    const BufferHandle constants = make_buffer(*device, kParamBytes, BufferUsage::CopyDestination | BufferUsage::Constant, "held constants");
    const BufferHandle output = make_buffer(*device, kElementBytes, BufferUsage::UnorderedAccess | BufferUsage::CopySource, "held output");
    const FenceHandle fence = take(device->create_fence(0));

    std::vector<float> values(kElements, 1.0f);
    const ComputeParams params{1.0f, 0.0f, kElements, 0};
    require_ok(device->upload_buffer(upload_input, 0, kElementBytes, values.data()));
    require_ok(device->upload_buffer(upload_params, 0, sizeof(params), &params));
    auto copy_in = take(device->create_command_list(QueueType::Copy));
    require_ok(copy_in->transition(input, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(copy_in->copy_buffer(upload_input, 0, input, 0, kElementBytes));
    require_ok(copy_in->transition(input, ResourceState::CopyDestination, ResourceState::Common));
    require_ok(copy_in->transition(constants, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(copy_in->copy_buffer(upload_params, 0, constants, 0, sizeof(params)));
    require_ok(copy_in->transition(constants, ResourceState::CopyDestination, ResourceState::Common));
    require_ok(copy_in->close());
    require_ok(submit_one(*device, QueueType::Copy, *copy_in, fence, 1));
    require_ok(device->queue_wait(QueueType::Compute, fence, 1));

    auto set = take(device->create_descriptor_set(compute.layout));
    require_ok(device->write_buffer(set, 0, 0, constants));
    require_ok(device->write_buffer(set, 1, 0, input));
    require_ok(device->write_buffer(set, 2, 0, output));
    const ResourceStats idle = take(device->resource_stats());
    auto dispatch = record_dispatch(*device, compute, set, constants, input, output);
    require_ok(submit_one(*device, QueueType::Compute, *dispatch, fence, 2));

    // Destroy the handles while the dispatch is still in flight. The handles go at once; the storage waits for the fence.
    require_ok(device->destroy(set));
    require_ok(device->destroy(output));
    require_ok(device->destroy(input));
    require_ok(device->destroy(constants));
    // The GPU may finish the dispatch before this line runs. The held-state checks apply only while it is still pending; the final
    // state below is checked either way.
    const ResourceStats in_flight = take(device->resource_stats());
    if (take(device->fence_value(fence)) < 2) {
        CHECK(in_flight.descriptor_sets == idle.descriptor_sets - 1);
        CHECK(in_flight.buffers == idle.buffers - 3);
        CHECK(in_flight.shader_descriptors == idle.shader_descriptors);
        CHECK(in_flight.retained_holds > 0);
    }

    require_ok(device->wait_fence(fence, 2));
    const ResourceStats done = take(device->resource_stats());
    CHECK(done.shader_descriptors == idle.shader_descriptors - 3);
    CHECK(done.retained_holds == 0);
    require_no_debug_errors(*device);
}

// ---- Ray tracing foundation (US-041) ----

namespace {

// One quad covering x in [-1, 0] and y in [-1, 1] at z = 0. dx12_raytrace.hlsl casts rays along +z from z = -1, so a ray hits the quad
// when its x is negative. Keep these values in step with tests/shaders/dx12_raytrace.hlsl.
constexpr std::array<float, 12> kQuadPositions{-1.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f};
constexpr std::array<std::uint32_t, 6> kQuadIndices{0, 1, 2, 0, 2, 3};
constexpr std::uint32_t kRayWidth = 8;
constexpr std::uint32_t kRayHeight = 4;
constexpr std::uint64_t kRayOutputBytes = kRayWidth * kRayHeight * sizeof(std::uint32_t);
constexpr std::uint64_t kQuadVertexBytes = sizeof(kQuadPositions);
constexpr std::uint64_t kQuadIndexBytes = sizeof(kQuadIndices);

// The RT suite needs hardware ray tracing. A device without it fails here rather than passing with nothing traced.
void require_ray_tracing(Device& device) {
    if (!device.capabilities().enabled(Feature::RayTracing)) {
        FAIL("ray tracing is not enabled on this adapter: " + std::string(feature_name(Feature::RayTracing)) + " is " +
             std::string(engine::rhi::to_string(device.capabilities().state(Feature::RayTracing))));
    }
}

void submit_and_wait(Device& device, CommandList& list, FenceHandle fence, std::uint64_t value) {
    std::vector<CommandList*> lists{&list};
    require_ok(device.submit(QueueType::Graphics, lists, fence, value));
    require_ok(device.wait_fence(fence, value));
}

// Uploads the quad through staging buffers and records the copies, then moves the buffers to ShaderRead, the state a BLAS build reads them in.
struct QuadBuffers {
    BufferHandle vertices;
    BufferHandle indices;
};

QuadBuffers stage_quad(Device& device, CommandList& list) {
    const BufferHandle vertex_upload = make_buffer(device, kQuadVertexBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "quad vertex upload");
    const BufferHandle index_upload = make_buffer(device, kQuadIndexBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "quad index upload");
    require_ok(device.upload_buffer(vertex_upload, 0, kQuadVertexBytes, kQuadPositions.data()));
    require_ok(device.upload_buffer(index_upload, 0, kQuadIndexBytes, kQuadIndices.data()));
    const QuadBuffers quad{
        make_buffer(device, kQuadVertexBytes, BufferUsage::CopyDestination | BufferUsage::Vertex | BufferUsage::ShaderResource, "quad vertices"),
        make_buffer(device, kQuadIndexBytes, BufferUsage::CopyDestination | BufferUsage::Index | BufferUsage::ShaderResource, "quad indices"),
    };
    require_ok(list.transition(quad.vertices, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(list.transition(quad.indices, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(list.copy_buffer(vertex_upload, 0, quad.vertices, 0, kQuadVertexBytes));
    require_ok(list.copy_buffer(index_upload, 0, quad.indices, 0, kQuadIndexBytes));
    require_ok(list.transition(quad.vertices, ResourceState::CopyDestination, ResourceState::ShaderRead));
    require_ok(list.transition(quad.indices, ResourceState::CopyDestination, ResourceState::ShaderRead));
    return quad;
}

BottomLevelDesc quad_geometry(const QuadBuffers& quad, bool allow_refit) {
    BottomLevelDesc desc;
    desc.vertex_buffer = quad.vertices;
    desc.vertex_stride = 3 * sizeof(float);
    desc.vertex_count = 4;
    desc.index_buffer = quad.indices;
    desc.index_format = IndexFormat::Uint32;
    desc.index_count = 6;
    desc.allow_refit = allow_refit;
    desc.debug_name = "quad";
    return desc;
}

// The ray output and its readback copy. A fresh pair is used for each dispatch so each one starts in the Common state.
struct RayOutput {
    BufferHandle output;
    BufferHandle readback;
};

RayOutput make_ray_output(Device& device) {
    return RayOutput{
        make_buffer(device, kRayOutputBytes, BufferUsage::UnorderedAccess | BufferUsage::CopySource, "ray output"),
        make_buffer(device, kRayOutputBytes, BufferUsage::CopyDestination | BufferUsage::CpuRead, "ray readback"),
    };
}

// The set binds the top-level structure at binding 0 and the output at binding 1, and the pipeline is the hit/miss library.
struct RayTrace {
    DescriptorSetHandle set;
    RayTracingPipelineHandle pipeline;
};

RayTrace make_ray_trace(Device& device, const RayOutput& out, AccelerationStructureHandle tlas) {
    DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings = {
        DescriptorBindingDesc{0, DescriptorType::ShaderResource, 1, ShaderStage::RayTracing},
        DescriptorBindingDesc{1, DescriptorType::UnorderedAccess, 1, ShaderStage::RayTracing},
    };
    const auto layout = take(device.create_descriptor_set_layout(layout_desc));
    const auto set = take(device.create_descriptor_set(layout));
    require_ok(device.write_acceleration_structure(set, 0, 0, tlas));
    require_ok(device.write_buffer(set, 1, 0, out.output));

    RayTracingPipelineDesc desc;
    desc.library = ShaderBinary{ShaderStage::None, "", load_shader("dx12_raytrace")};
    desc.raygen_entry = "RayGen";
    desc.miss_entry = "Miss";
    desc.closest_hit_entry = "ClosestHit";
    desc.set_layouts = {layout};
    desc.debug_name = "hit and miss";
    return RayTrace{set, take(device.create_ray_tracing_pipeline(desc))};
}

// Records one dispatch over the output and the copy back to the readback buffer. The output starts in Common.
void record_ray_trace(CommandList& list, const RayTrace& trace, const RayOutput& out) {
    require_ok(list.transition(out.output, ResourceState::Common, ResourceState::UnorderedAccess));
    require_ok(list.set_ray_tracing_pipeline(trace.pipeline));
    require_ok(list.set_descriptor_set(0, trace.set));
    require_ok(list.dispatch_rays(kRayWidth, kRayHeight, 1));
    require_ok(list.transition(out.output, ResourceState::UnorderedAccess, ResourceState::CopySource));
    require_ok(list.copy_buffer(out.output, 0, out.readback, 0, kRayOutputBytes));
}

std::vector<std::uint32_t> read_ray_output(Device& device, const RayOutput& out) {
    std::vector<std::uint32_t> values(kRayWidth * kRayHeight);
    require_ok(device.read_buffer(out.readback, 0, kRayOutputBytes, values.data()));
    return values;
}

// True when every pixel reads 1 on the side the geometry covers and 0 elsewhere. shifted_right is the geometry moved by +1 in x.
bool hit_pattern_matches(const std::vector<std::uint32_t>& values, bool shifted_right) {
    for (std::uint32_t y = 0; y < kRayHeight; ++y) {
        for (std::uint32_t x = 0; x < kRayWidth; ++x) {
            const bool right = x >= kRayWidth / 2;
            const std::uint32_t expected = (shifted_right ? right : !right) ? 1u : 0u;
            if (values[y * kRayWidth + x] != expected) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("DX12 ray tracing is gated by its feature and rejects descriptions it cannot build", "[gpu][dx12]") {
    DeviceDesc forced_off = debug_dx12();
    forced_off.disabled_features = Feature::RayTracing;
    auto disabled = take(engine::rhi::create_device(forced_off));
    CHECK(disabled->capabilities().state(Feature::RayTracing) == FeatureState::DisabledForTest);
    CHECK(fails_with(disabled->create_bottom_level(BottomLevelDesc{}), RhiErrorCode::FeatureDisabled));
    CHECK(fails_with(disabled->create_top_level(TopLevelDesc{1, false, ""}), RhiErrorCode::FeatureDisabled));
    CHECK(fails_with(disabled->create_ray_tracing_pipeline(RayTracingPipelineDesc{}), RhiErrorCode::FeatureDisabled));

    auto device = take(engine::rhi::create_device(debug_dx12()));
    require_ray_tracing(*device);
    CHECK(fails_with(device->create_top_level(TopLevelDesc{0, false, ""}), RhiErrorCode::InvalidDescriptor));
    CHECK(fails_with(device->create_bottom_level(BottomLevelDesc{}), RhiErrorCode::InvalidHandle));

    // A vertex buffer without Vertex usage, and an index count that does not name whole triangles, are both rejected.
    const BufferHandle plain = make_buffer(*device, kQuadVertexBytes, BufferUsage::CopyDestination | BufferUsage::ShaderResource, "plain");
    BottomLevelDesc not_vertex;
    not_vertex.vertex_buffer = plain;
    not_vertex.vertex_stride = 12;
    not_vertex.vertex_count = 4;
    CHECK(fails_with(device->create_bottom_level(not_vertex), RhiErrorCode::InvalidDescriptor));

    const BufferHandle vertices = make_buffer(*device, kQuadVertexBytes, BufferUsage::CopyDestination | BufferUsage::Vertex | BufferUsage::ShaderResource, "vertices");
    const BufferHandle indices = make_buffer(*device, kQuadIndexBytes, BufferUsage::CopyDestination | BufferUsage::Index | BufferUsage::ShaderResource, "indices");
    BottomLevelDesc ragged;
    ragged.vertex_buffer = vertices;
    ragged.vertex_stride = 12;
    ragged.vertex_count = 4;
    ragged.index_buffer = indices;
    ragged.index_count = 5;
    CHECK(fails_with(device->create_bottom_level(ragged), RhiErrorCode::InvalidDescriptor));
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 traces a hit and a miss through a bottom-level and a top-level structure", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    require_ray_tracing(*device);
    auto list = take(device->create_command_list(QueueType::Graphics));
    const QuadBuffers quad = stage_quad(*device, *list);
    const auto blas = take(device->create_bottom_level(quad_geometry(quad, false)));
    TopLevelDesc top;
    top.max_instances = 1;
    top.debug_name = "scene";
    const auto tlas = take(device->create_top_level(top));
    require_ok(device->write_top_level(tlas, {TopLevelInstance{blas}}));

    const RayOutput out = make_ray_output(*device);
    const RayTrace trace = make_ray_trace(*device, out, tlas);
    require_ok(list->build_bottom_level(blas, BuildMode::Build));
    require_ok(list->build_top_level(tlas, BuildMode::Build));
    record_ray_trace(*list, trace, out);
    require_ok(list->close());
    const auto fence = take(device->create_fence(0));
    submit_and_wait(*device, *list, fence, 1);

    CHECK(hit_pattern_matches(read_ray_output(*device, out), false));
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 refits a moved instance, and a top-level build rejects a destroyed bottom-level structure", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    require_ray_tracing(*device);
    auto list = take(device->create_command_list(QueueType::Graphics));
    const QuadBuffers quad = stage_quad(*device, *list);
    const auto blas = take(device->create_bottom_level(quad_geometry(quad, true)));
    TopLevelDesc top;
    top.max_instances = 1;
    top.allow_refit = true;
    const auto tlas = take(device->create_top_level(top));
    require_ok(device->write_top_level(tlas, {TopLevelInstance{blas}}));
    const auto fence = take(device->create_fence(0));

    const RayOutput first = make_ray_output(*device);
    const RayTrace first_trace = make_ray_trace(*device, first, tlas);
    require_ok(list->build_bottom_level(blas, BuildMode::Build));
    require_ok(list->build_top_level(tlas, BuildMode::Build));
    record_ray_trace(*list, first_trace, first);
    require_ok(list->close());
    submit_and_wait(*device, *list, fence, 1);
    CHECK(hit_pattern_matches(read_ray_output(*device, first), false));

    // The instance moves +1 in x. The geometry and the instance count are unchanged, so the top-level structure is refitted in place.
    TopLevelInstance moved{blas};
    moved.transform[3] = 1.0f;
    require_ok(device->write_top_level(tlas, {moved}));
    auto refit_list = take(device->create_command_list(QueueType::Graphics));
    const RayOutput second = make_ray_output(*device);
    const RayTrace second_trace = make_ray_trace(*device, second, tlas);
    require_ok(refit_list->build_top_level(tlas, BuildMode::Refit));
    record_ray_trace(*refit_list, second_trace, second);
    require_ok(refit_list->close());
    submit_and_wait(*device, *refit_list, fence, 2);
    CHECK(hit_pattern_matches(read_ray_output(*device, second), true));

    // Once the bottom-level structure is destroyed, the instance list cannot name it: writes and builds that use it are rejected.
    require_ok(device->destroy(blas));
    CHECK(fails_with(device->write_top_level(tlas, {TopLevelInstance{blas}}), RhiErrorCode::InvalidHandle));
    auto stale_list = take(device->create_command_list(QueueType::Graphics));
    CHECK(fails_with(stale_list->build_top_level(tlas, BuildMode::Refit), RhiErrorCode::InvalidHandle));
    require_no_debug_errors(*device);
}

TEST_CASE("DX12 destroys ray tracing objects during an unfinished trace without invalid access", "[gpu][dx12]") {
    auto device = take(engine::rhi::create_device(debug_dx12()));
    require_ray_tracing(*device);
    auto list = take(device->create_command_list(QueueType::Graphics));
    const QuadBuffers quad = stage_quad(*device, *list);
    const auto blas = take(device->create_bottom_level(quad_geometry(quad, false)));
    TopLevelDesc top;
    top.max_instances = 1;
    const auto tlas = take(device->create_top_level(top));
    require_ok(device->write_top_level(tlas, {TopLevelInstance{blas}}));
    const RayOutput out = make_ray_output(*device);
    const RayTrace trace = make_ray_trace(*device, out, tlas);
    require_ok(list->build_bottom_level(blas, BuildMode::Build));
    require_ok(list->build_top_level(tlas, BuildMode::Build));
    record_ray_trace(*list, trace, out);
    require_ok(list->close());
    const auto fence = take(device->create_fence(0));
    std::vector<CommandList*> lists{list.get()};
    require_ok(device->submit(QueueType::Graphics, lists, fence, 1));

    // The submission has not completed. Every object it names is destroyed now; the memory must stay valid until the fence.
    require_ok(device->destroy(trace.pipeline));
    require_ok(device->destroy(trace.set));
    require_ok(device->destroy(tlas));
    require_ok(device->destroy(blas));
    require_ok(device->destroy(out.output));
    require_ok(device->wait_fence(fence, 1));

    CHECK(hit_pattern_matches(read_ray_output(*device, out), false));
    CHECK(fails_with(device->write_top_level(tlas, {}), RhiErrorCode::InvalidHandle));
    require_no_debug_errors(*device);
}
