// Compiled shaders on the GPU (US-031). ShaderProgram compiles the fixture with the pinned DXC into the backend's target, reflects
// it, and builds the pipeline. The dispatch must produce the reference values on real hardware, and a reload that fails must leave
// the pipeline that is running usable. The debug layers stay on, so validation errors fail the test.

#include "engine/rhi/device.hpp"
#include "engine/shader/pipeline_cache.hpp"
#include "engine/shader/shader_program.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using engine::rhi::Backend;
using engine::rhi::BufferDesc;
using engine::rhi::BufferHandle;
using engine::rhi::BufferUsage;
using engine::rhi::CommandList;
using engine::rhi::DescriptorBindingDesc;
using engine::rhi::DescriptorSetLayoutDesc;
using engine::rhi::DescriptorSetLayoutHandle;
using engine::rhi::DescriptorType;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::FenceHandle;
using engine::rhi::PipelineHandle;
using engine::rhi::QueueType;
using engine::rhi::ResourceState;
using engine::rhi::RhiResult;
using engine::rhi::RhiStatus;
using engine::rhi::ShaderStage;
using engine::shader::CpuBlockLayout;
using engine::shader::make_environment;
using engine::shader::PipelineCache;
using engine::shader::ReflectedMember;
using engine::shader::ReloadReport;
using engine::shader::ShaderCompiler;
using engine::shader::ShaderProgram;
using engine::shader::ShaderProgramDesc;
using engine::shader::ShaderTarget;

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kElements = 256;
constexpr std::uint64_t kElementBytes = kElements * sizeof(float);
constexpr std::uint64_t kParamBytes = 16;

// Keep in step with the Params cbuffer in tests/shaders/dx12_compute.hlsl.
struct ComputeParams {
    float scale = 1.0f;
    float offset = 0.0f;
    std::uint32_t count = 0;
    std::uint32_t pad = 0;
};

// The same shader as the US-029 fixture, with its source copied into the scratch directory so a test can edit it.
constexpr const char* kShader =
    "cbuffer Params : register(b0) { float Scale; float Offset; uint Count; uint Pad; };\n"
    "ByteAddressBuffer Input : register(t1);\n"
    "RWByteAddressBuffer Output : register(u2);\n"
    "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < Count) {\n"
    "        float value = asfloat(Input.Load(id.x * 4));\n"
    "        Output.Store(id.x * 4, asuint(value * Scale + Offset));\n"
    "    }\n"
    "}\n";

constexpr const char* kBrokenShader =
    "cbuffer Params : register(b0) { float Scale; float Offset; uint Count; uint Pad; };\n"
    "ByteAddressBuffer Input : register(t1);\n"
    "RWByteAddressBuffer Output : register(u2);\n"
    "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { Output.Store(0, Missing); }\n";

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

void require_no_debug_errors(Device& device) {
    const RhiStatus status = device.check_debug_messages();
    if (!status.ok()) {
        FAIL(status.error().message);
    }
}

DeviceDesc debug_device(Backend backend) {
    DeviceDesc desc;
    desc.backend = backend;
    desc.debug_layers = true;
    return desc;
}

BufferHandle make_buffer(Device& device, std::uint64_t size, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size_bytes = size;
    desc.usage = usage;
    desc.debug_name = name;
    return take(device.create_buffer(desc));
}

void write_text(const fs::path& path, const char* text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Uploads the inputs, dispatches the program's pipeline, and reads the output back. Each queue waits on the GPU for the one before
// it, so the CPU never blocks in the middle.
std::vector<float> dispatch_round_trip(Device& device, PipelineHandle pipeline, DescriptorSetLayoutHandle layout,
                                       const std::vector<float>& values, const ComputeParams& params) {
    const BufferHandle upload_input = make_buffer(device, kElementBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "upload input");
    const BufferHandle upload_params = make_buffer(device, kParamBytes, BufferUsage::CpuWrite | BufferUsage::CopySource, "upload params");
    const BufferHandle input = make_buffer(device, kElementBytes, BufferUsage::CopyDestination | BufferUsage::ShaderResource, "input");
    const BufferHandle constants = make_buffer(device, kParamBytes, BufferUsage::CopyDestination | BufferUsage::Constant, "constants");
    const BufferHandle output = make_buffer(device, kElementBytes, BufferUsage::UnorderedAccess | BufferUsage::CopySource, "output");
    const BufferHandle readback = make_buffer(device, kElementBytes, BufferUsage::CopyDestination | BufferUsage::CpuRead, "readback");
    const FenceHandle fence = take(device.create_fence(0));

    require_ok(device.upload_buffer(upload_input, 0, kElementBytes, values.data()));
    require_ok(device.upload_buffer(upload_params, 0, sizeof(params), &params));

    auto copy_in = take(device.create_command_list(QueueType::Copy));
    require_ok(copy_in->transition(input, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(copy_in->copy_buffer(upload_input, 0, input, 0, kElementBytes));
    require_ok(copy_in->transition(input, ResourceState::CopyDestination, ResourceState::Common));
    require_ok(copy_in->transition(constants, ResourceState::Common, ResourceState::CopyDestination));
    require_ok(copy_in->copy_buffer(upload_params, 0, constants, 0, sizeof(params)));
    require_ok(copy_in->transition(constants, ResourceState::CopyDestination, ResourceState::Common));
    require_ok(copy_in->close());
    const std::vector<CommandList*> copy_lists{copy_in.get()};
    require_ok(device.submit(QueueType::Copy, copy_lists, fence, 1));

    require_ok(device.queue_wait(QueueType::Compute, fence, 1));
    auto set = take(device.create_descriptor_set(layout));
    require_ok(device.write_buffer(set, 0, 0, constants));
    require_ok(device.write_buffer(set, 1, 0, input));
    require_ok(device.write_buffer(set, 2, 0, output));

    auto dispatch = take(device.create_command_list(QueueType::Compute));
    require_ok(dispatch->transition(constants, ResourceState::Common, ResourceState::ConstantBuffer));
    require_ok(dispatch->transition(input, ResourceState::Common, ResourceState::ShaderRead));
    require_ok(dispatch->transition(output, ResourceState::Common, ResourceState::UnorderedAccess));
    require_ok(dispatch->set_pipeline(pipeline));
    require_ok(dispatch->set_descriptor_set(0, set));
    require_ok(dispatch->dispatch(kElements / 64, 1, 1));
    require_ok(dispatch->transition(output, ResourceState::UnorderedAccess, ResourceState::Common));
    require_ok(dispatch->close());
    const std::vector<CommandList*> dispatch_lists{dispatch.get()};
    require_ok(device.submit(QueueType::Compute, dispatch_lists, fence, 2));

    require_ok(device.queue_wait(QueueType::Copy, fence, 2));
    auto copy_out = take(device.create_command_list(QueueType::Copy));
    require_ok(copy_out->transition(output, ResourceState::Common, ResourceState::CopySource));
    require_ok(copy_out->copy_buffer(output, 0, readback, 0, kElementBytes));
    require_ok(copy_out->close());
    const std::vector<CommandList*> out_lists{copy_out.get()};
    require_ok(device.submit(QueueType::Copy, out_lists, fence, 3));

    require_ok(device.wait_fence(fence, 3));
    std::vector<float> result(kElements, -1.0f);
    require_ok(device.read_buffer(readback, 0, kElementBytes, result.data()));
    return result;
}

void run_compiled_program(Backend backend, ShaderTarget target, const char* scratch_name) {
    const fs::path dir = fs::path(ENGINE_TEST_SHADER_SCRATCH) / scratch_name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path source = dir / "compute.hlsl";
    write_text(source, kShader);

    auto device = take(engine::rhi::create_device(debug_device(backend)));
    auto compiler_result = ShaderCompiler::create(ENGINE_TEST_DXC_PATH, dir.string());
    REQUIRE(compiler_result.ok());
    const ShaderCompiler compiler = std::move(compiler_result).value();
    PipelineCache cache(make_environment(compiler, device->capabilities()));

    DescriptorSetLayoutDesc layout_desc;
    layout_desc.bindings = {
        DescriptorBindingDesc{0, DescriptorType::ConstantBuffer, 1, ShaderStage::Compute},
        DescriptorBindingDesc{1, DescriptorType::ShaderResource, 1, ShaderStage::Compute},
        DescriptorBindingDesc{2, DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
    };
    const DescriptorSetLayoutHandle layout = take(device->create_descriptor_set_layout(layout_desc));

    ShaderProgramDesc desc;
    desc.debug_name = "US-031 compiled compute";
    desc.request.source_path = source.string();
    desc.request.entry_point = "main";
    desc.request.profile = "cs_6_0";
    desc.request.target = target;
    desc.layouts = {layout_desc};
    desc.layout_handles = {layout};
    CpuBlockLayout params;
    params.name = "Params";
    params.size_bytes = 16;
    params.members = {ReflectedMember{"Scale", 0}, ReflectedMember{"Offset", 4}, ReflectedMember{"Count", 8}, ReflectedMember{"Pad", 12}};
    desc.cpu_blocks = {params};
    ShaderProgram program(std::move(desc), compiler, cache);

    const ReloadReport built = program.reload(*device);
    REQUIRE(built.replaced);
    REQUIRE(program.pipeline().valid());

    std::vector<float> values(kElements);
    for (std::uint32_t i = 0; i < kElements; ++i) {
        values[i] = static_cast<float>(i) * 0.5f;
    }
    const ComputeParams first{2.0f, 1.0f, kElements, 0};
    const std::vector<float> produced = dispatch_round_trip(*device, program.pipeline(), layout, values, first);
    for (std::uint32_t i = 0; i < kElements; ++i) {
        CHECK(produced[i] == values[i] * first.scale + first.offset);
    }
    require_no_debug_errors(*device);

    // A broken edit fails to reload. The pipeline that is running must still dispatch, with the reference values intact.
    const PipelineHandle running = program.pipeline();
    write_text(source, kBrokenShader);
    CHECK_FALSE(program.reload(*device).replaced);
    CHECK(program.pipeline() == running);

    const ComputeParams second{3.0f, -2.0f, kElements, 0};
    const std::vector<float> after = dispatch_round_trip(*device, running, layout, values, second);
    for (std::uint32_t i = 0; i < kElements; ++i) {
        CHECK(after[i] == values[i] * second.scale + second.offset);
    }
    require_no_debug_errors(*device);
    REQUIRE(program.release(*device).ok());
}

}  // namespace

TEST_CASE("DX12 runs a shader built by the shader pipeline, and keeps its pipeline across a failed reload", "[gpu][dx12]") {
    run_compiled_program(Backend::Dx12, ShaderTarget::Dxil, "gpu_dx12");
}

TEST_CASE("Vulkan runs a shader built by the shader pipeline, and keeps its pipeline across a failed reload", "[gpu][vulkan]") {
    run_compiled_program(Backend::Vulkan, ShaderTarget::SpirV, "gpu_vulkan");
}
