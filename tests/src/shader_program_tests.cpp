// Reloadable compute programs (US-031) on the CPU reference device: the shader is compiled by the pinned DXC, its reflection is
// checked against the declared layout, and the pipeline goes through the device's contract. A reload that fails must keep the
// last valid pipeline, and a reload that succeeds retires the old one.

#include "engine/rhi/device.hpp"
#include "engine/rhi/error.hpp"
#include "engine/shader/pipeline_cache.hpp"
#include "engine/shader/shader_program.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

using engine::rhi::DescriptorBindingDesc;
using engine::rhi::DescriptorSetLayoutDesc;
using engine::rhi::DescriptorType;
using engine::rhi::Device;
using engine::rhi::DeviceDesc;
using engine::rhi::PipelineHandle;
using engine::rhi::QueueType;
using engine::rhi::RhiErrorCode;
using engine::rhi::ShaderStage;
using engine::shader::CompileRequest;
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

constexpr const char* kGoodShader =
    "cbuffer Params : register(b0) { float Scale; float Offset; uint Count; uint Pad; };\n"
    "RWStructuredBuffer<float> Output : register(u2);\n"
    "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < Count) { Output[id.x] = Scale; }\n"
    "}\n";

// The error is on line 3: `Missing` is not declared.
constexpr const char* kBrokenShader =
    "cbuffer Params : register(b0) { float Scale; float Offset; uint Count; uint Pad; };\n"
    "RWStructuredBuffer<float> Output : register(u2);\n"
    "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { Output[id.x] = Missing; }\n";

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(ENGINE_TEST_SHADER_SCRATCH) / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

struct Harness {
    std::unique_ptr<Device> device;
    std::unique_ptr<ShaderCompiler> compiler;
    std::unique_ptr<PipelineCache> cache;
    fs::path dir;
    fs::path source;
    DescriptorSetLayoutDesc layout;
    engine::rhi::DescriptorSetLayoutHandle layout_handle;
};

Harness make_harness(const std::string& name, const std::string& shader) {
    Harness harness;
    harness.dir = scratch(name);
    harness.source = harness.dir / "compute.hlsl";
    write_text(harness.source, shader);

    auto device = engine::rhi::create_device(DeviceDesc{});
    REQUIRE(device.ok());
    harness.device = std::move(device).value();

    auto compiler = ShaderCompiler::create(ENGINE_TEST_DXC_PATH, harness.dir.string());
    REQUIRE(compiler.ok());
    harness.compiler = std::make_unique<ShaderCompiler>(std::move(compiler).value());
    harness.cache = std::make_unique<PipelineCache>(make_environment(*harness.compiler, harness.device->capabilities()));

    harness.layout.bindings = {
        DescriptorBindingDesc{0, DescriptorType::ConstantBuffer, 1, ShaderStage::Compute},
        DescriptorBindingDesc{1, DescriptorType::ShaderResource, 1, ShaderStage::Compute},
        DescriptorBindingDesc{2, DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
    };
    auto layout = harness.device->create_descriptor_set_layout(harness.layout);
    REQUIRE(layout.ok());
    harness.layout_handle = layout.value();
    return harness;
}

ShaderProgramDesc program_desc(const Harness& harness) {
    ShaderProgramDesc desc;
    desc.debug_name = "compute";
    desc.request.source_path = harness.source.string();
    desc.request.entry_point = "main";
    desc.request.profile = "cs_6_0";
    desc.request.target = ShaderTarget::Dxil;
    desc.layouts = {harness.layout};
    desc.layout_handles = {harness.layout_handle};
    CpuBlockLayout params;
    params.name = "Params";
    params.set = 0;
    params.binding = 0;
    params.size_bytes = 16;
    params.members = {ReflectedMember{"Scale", 0}, ReflectedMember{"Offset", 4}, ReflectedMember{"Count", 8}, ReflectedMember{"Pad", 12}};
    desc.cpu_blocks = {params};
    return desc;
}

// True when the status failed with InvalidHandle. Reads the code only after checking the status, so a success is a plain failure.
bool invalid_handle(const engine::rhi::RhiStatus& status) {
    return !status.ok() && status.error().code == RhiErrorCode::InvalidHandle;
}

bool has_error_with(const ReloadReport& report, const std::string& text) {
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity == engine::shader::Severity::Error && diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("A compute program builds its pipeline from compiled bytecode and records it in the cache", "[integration][shader]") {
    Harness h = make_harness("program_build", kGoodShader);
    ShaderProgram program(program_desc(h), *h.compiler, *h.cache);
    CHECK_FALSE(program.pipeline().valid());

    const ReloadReport report = program.reload(*h.device);
    CHECK(report.replaced);
    CHECK_FALSE(report.from_cache);
    CHECK(report.diagnostics.empty());
    CHECK(program.pipeline().valid());
    CHECK(h.cache->size() == 1);

    auto list = h.device->create_command_list(QueueType::Compute);
    REQUIRE(list.ok());
    CHECK(list.value()->set_pipeline(program.pipeline()).ok());
}

TEST_CASE("A second program with the same inputs is served from the cache and not recompiled", "[integration][shader]") {
    Harness h = make_harness("program_cache_hit", kGoodShader);
    ShaderProgram first(program_desc(h), *h.compiler, *h.cache);
    REQUIRE(first.reload(*h.device).replaced);

    // The cache is persisted and reloaded, as it is between runs.
    const fs::path file = h.dir / "pipelines.cache";
    REQUIRE(h.cache->save(file.string()).ok());
    auto loaded = PipelineCache::load(file.string(), h.cache->environment());
    REQUIRE(loaded.ok());
    PipelineCache restored = std::move(loaded).value();

    ShaderProgram second(program_desc(h), *h.compiler, restored);
    const ReloadReport report = second.reload(*h.device);
    CHECK(report.replaced);
    CHECK(report.from_cache);
}

TEST_CASE("A cache entry is invalidated when an included file changes", "[integration][shader]") {
    Harness h = make_harness("program_include", "#include \"defs.hlsli\"\n" + std::string(kGoodShader));
    write_text(h.dir / "defs.hlsli", "// version 1\n");
    ShaderProgram program(program_desc(h), *h.compiler, *h.cache);
    REQUIRE(program.reload(*h.device).replaced);

    CHECK(program.reload(*h.device).from_cache);

    write_text(h.dir / "defs.hlsli", "// version 2\n");
    const ReloadReport after_edit = program.reload(*h.device);
    CHECK(after_edit.replaced);
    CHECK_FALSE(after_edit.from_cache);
}

TEST_CASE("A reload that fails to compile keeps the last valid pipeline and reports the error", "[integration][shader]") {
    Harness h = make_harness("program_keep_last", kGoodShader);
    ShaderProgram program(program_desc(h), *h.compiler, *h.cache);
    REQUIRE(program.reload(*h.device).replaced);
    const PipelineHandle good = program.pipeline();

    write_text(h.source, kBrokenShader);
    const ReloadReport failed = program.reload(*h.device);
    CHECK_FALSE(failed.replaced);
    CHECK(program.pipeline() == good);
    CHECK(has_error_with(failed, "Missing"));
    CHECK(program.diagnostics_json().find("\"status\": \"error\"") != std::string::npos);
    CHECK(program.diagnostics_json().find("\"line\": 3") != std::string::npos);

    // The old pipeline is still usable on the device.
    auto list = h.device->create_command_list(QueueType::Compute);
    REQUIRE(list.ok());
    CHECK(list.value()->set_pipeline(good).ok());
}

TEST_CASE("A fixed source replaces the pipeline and retires the previous one", "[integration][shader]") {
    Harness h = make_harness("program_replace", kGoodShader);
    ShaderProgram program(program_desc(h), *h.compiler, *h.cache);
    REQUIRE(program.reload(*h.device).replaced);
    const PipelineHandle first = program.pipeline();

    write_text(h.source, kBrokenShader);
    REQUIRE_FALSE(program.reload(*h.device).replaced);

    write_text(h.source, std::string(kGoodShader) + "// edited\n");
    const ReloadReport fixed = program.reload(*h.device);
    CHECK(fixed.replaced);
    CHECK(program.pipeline().valid());
    CHECK_FALSE(program.pipeline() == first);

    auto list = h.device->create_command_list(QueueType::Compute);
    REQUIRE(list.ok());
    CHECK(invalid_handle(list.value()->set_pipeline(first)));
}

TEST_CASE("Bytecode that disagrees with the declared layout fails the reload before any pipeline is created", "[integration][shader]") {
    Harness h = make_harness("program_layout", kGoodShader);
    ShaderProgramDesc desc = program_desc(h);
    // The shader writes through a UAV at u2. Declaring it as a read-only SRV must be caught here, not at the first dispatch.
    desc.layouts[0].bindings[2].type = DescriptorType::ShaderResource;
    ShaderProgram program(std::move(desc), *h.compiler, *h.cache);

    const ReloadReport report = program.reload(*h.device);
    CHECK_FALSE(report.replaced);
    CHECK_FALSE(program.pipeline().valid());
    CHECK(has_error_with(report, "different descriptor type"));
}

TEST_CASE("A program releases its pipeline, and a released program has no pipeline", "[integration][shader]") {
    Harness h = make_harness("program_release", kGoodShader);
    ShaderProgram program(program_desc(h), *h.compiler, *h.cache);
    REQUIRE(program.reload(*h.device).replaced);
    const PipelineHandle current = program.pipeline();

    CHECK(program.release(*h.device).ok());
    CHECK_FALSE(program.pipeline().valid());
    auto list = h.device->create_command_list(QueueType::Compute);
    REQUIRE(list.ok());
    CHECK(invalid_handle(list.value()->set_pipeline(current)));
    CHECK(program.release(*h.device).ok());
}

TEST_CASE("A program rejects a profile that is not a compute profile", "[integration][shader]") {
    Harness h = make_harness("program_profile", kGoodShader);
    ShaderProgramDesc desc = program_desc(h);
    desc.request.profile = "ps_6_0";
    ShaderProgram program(std::move(desc), *h.compiler, *h.cache);
    const ReloadReport report = program.reload(*h.device);
    CHECK_FALSE(report.replaced);
    CHECK(has_error_with(report, "is not a compute profile"));
}
