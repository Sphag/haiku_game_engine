// HLSL compilation through the pinned DXC (US-031). These run the real compiler that the build staged, so they check the same
// output the GPU suites load: DXIL and SPIR-V from one source, the reflection of each, and the diagnostics of a bad source.

#include "engine/rhi/types.hpp"
#include "engine/shader/compiler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using engine::rhi::DescriptorBindingDesc;
using engine::rhi::DescriptorSetLayoutDesc;
using engine::rhi::DescriptorType;
using engine::rhi::ShaderStage;
using engine::shader::BindingClass;
using engine::shader::compare_reflections;
using engine::shader::CompileRequest;
using engine::shader::CompileResult;
using engine::shader::CpuBlockLayout;
using engine::shader::diagnostics_to_json;
using engine::shader::diagnostics_to_text;
using engine::shader::Diagnostic;
using engine::shader::has_errors;
using engine::shader::ReflectedMember;
using engine::shader::Severity;
using engine::shader::ShaderCompiler;
using engine::shader::ShaderDefine;
using engine::shader::ShaderTarget;
using engine::shader::validate_bindings;
using engine::shader::validate_cpu_layouts;

namespace fs = std::filesystem;

namespace {

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

ShaderCompiler make_compiler(const fs::path& dir) {
    auto created = ShaderCompiler::create(ENGINE_TEST_DXC_PATH, dir.string());
    if (!created.ok()) {
        FAIL(created.error());
    }
    return std::move(created).value();
}

CompileRequest request_for(const std::string& source, ShaderTarget target) {
    CompileRequest request;
    request.source_path = source;
    request.entry_point = "main";
    request.profile = "cs_6_0";
    request.target = target;
    return request;
}

std::string source_path(const char* name) {
    return (fs::path(ENGINE_TEST_SHADER_SOURCE_DIR) / name).string();
}

}  // namespace

TEST_CASE("The pinned compiler turns the fixture into DXIL and SPIR-V with matching reflection", "[integration][shader]") {
    const ShaderCompiler compiler = make_compiler(scratch("compiler_fixture"));
    CHECK(compiler.fingerprint().size() == 64);

    const CompileResult dxil = compiler.compile(request_for(source_path("dx12_compute.hlsl"), ShaderTarget::Dxil));
    REQUIRE(dxil.ok);
    CHECK(dxil.diagnostics.empty());
    REQUIRE(dxil.bytecode.size() > 4);
    CHECK(std::string(dxil.bytecode.begin(), dxil.bytecode.begin() + 4) == "DXBC");
    REQUIRE(!dxil.dependencies.empty());
    CHECK(fs::path(dxil.dependencies[0]).filename() == "dx12_compute.hlsl");

    const CompileResult spirv = compiler.compile(request_for(source_path("dx12_compute.hlsl"), ShaderTarget::SpirV));
    REQUIRE(spirv.ok);
    REQUIRE(spirv.bytecode.size() > 4);
    // SPIR-V magic 0x07230203, stored little-endian.
    CHECK(spirv.bytecode[0] == 0x03);
    CHECK(spirv.bytecode[1] == 0x02);
    CHECK(spirv.bytecode[2] == 0x23);
    CHECK(spirv.bytecode[3] == 0x07);

    // One source, two targets: the bindings and the constant buffer offsets must be the same.
    CHECK(compare_reflections("dx12_compute.hlsl", dxil.reflection, spirv.reflection).empty());

    const std::vector<DescriptorSetLayoutDesc> layouts = {DescriptorSetLayoutDesc{{
        DescriptorBindingDesc{0, DescriptorType::ConstantBuffer, 1, ShaderStage::Compute},
        DescriptorBindingDesc{1, DescriptorType::ShaderResource, 1, ShaderStage::Compute},
        DescriptorBindingDesc{2, DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
    }}};
    CHECK(validate_bindings("dx12_compute.hlsl", dxil.reflection, layouts).empty());
    CHECK(validate_bindings("dx12_compute.hlsl", spirv.reflection, layouts).empty());

    // The CPU struct, laid out with offsetof, must match the GPU's Params on both targets.
    CpuBlockLayout params;
    params.name = "Params";
    params.size_bytes = 16;
    params.members = {ReflectedMember{"Scale", 0}, ReflectedMember{"Offset", 4}, ReflectedMember{"Count", 8}, ReflectedMember{"Pad", 12}};
    CHECK(validate_cpu_layouts("dx12_compute.hlsl", dxil.reflection, {params}).empty());
    CHECK(validate_cpu_layouts("dx12_compute.hlsl", spirv.reflection, {params}).empty());

    REQUIRE(dxil.reflection.bindings.size() == 3);
    CHECK(dxil.reflection.bindings[1].cls == BindingClass::ShaderResource);
}

TEST_CASE("A bad source fails with its file and line in the text and JSON reports, and no bytecode", "[integration][shader]") {
    const ShaderCompiler compiler = make_compiler(scratch("compiler_error"));
    const CompileResult result = compiler.compile(request_for(source_path("fixture_error.hlsl"), ShaderTarget::Dxil));
    CHECK_FALSE(result.ok);
    CHECK(result.bytecode.empty());
    CHECK(has_errors(result.diagnostics));

    bool located = false;
    for (const Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.severity == Severity::Error && diagnostic.line == 7) {
            located = fs::path(diagnostic.file).filename() == "fixture_error.hlsl" && diagnostic.column > 0;
        }
    }
    CHECK(located);

    const std::string text = diagnostics_to_text(result.diagnostics);
    CHECK(text.find("fixture_error.hlsl:7:") != std::string::npos);
    CHECK(text.find("error:") != std::string::npos);
    CHECK(diagnostics_to_json(source_path("fixture_error.hlsl"), result.diagnostics).find("\"status\": \"error\"") != std::string::npos);
}

TEST_CASE("A missing source is reported as an error before the compiler runs", "[integration][shader]") {
    const ShaderCompiler compiler = make_compiler(scratch("compiler_missing"));
    const CompileResult result = compiler.compile(request_for(source_path("no_such_shader.hlsl"), ShaderTarget::Dxil));
    CHECK_FALSE(result.ok);
    REQUIRE(!result.diagnostics.empty());
    CHECK(result.diagnostics[0].message.find("source file not found") != std::string::npos);
}

TEST_CASE("Defines reach the compiler, and includes are recorded as dependencies", "[integration][shader]") {
    const fs::path dir = scratch("compiler_defines_includes");
    write_text(dir / "common.hlsli", "#define SCALE 2\n");
    write_text(dir / "main.hlsl",
               "#include \"common.hlsli\"\n"
               "#ifndef MODE\n"
               "#error MODE must be defined\n"
               "#endif\n"
               "RWStructuredBuffer<float> Out : register(u0);\n"
               "[numthreads(1, 1, 1)] void main() { Out[0] = SCALE; }\n");
    const ShaderCompiler compiler = make_compiler(dir);

    const CompileResult missing_define = compiler.compile(request_for((dir / "main.hlsl").string(), ShaderTarget::Dxil));
    CHECK_FALSE(missing_define.ok);
    CHECK(diagnostics_to_text(missing_define.diagnostics).find("MODE must be defined") != std::string::npos);

    CompileRequest defined = request_for((dir / "main.hlsl").string(), ShaderTarget::Dxil);
    defined.defines = {ShaderDefine{"MODE", "1"}};
    const CompileResult ok = compiler.compile(defined);
    REQUIRE(ok.ok);
    bool saw_include = false;
    for (const std::string& dependency : ok.dependencies) {
        saw_include = saw_include || fs::path(dependency).filename() == "common.hlsli";
    }
    CHECK(saw_include);
}
