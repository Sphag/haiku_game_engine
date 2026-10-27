// Shader reflection (US-031) on listings. The listings below use the layout that the pinned DXC prints (dxc -Fc), so the parsers
// are checked without running a compiler. shader_compiler_tests.cpp checks the same rules on real compiler output.

#include "engine/rhi/types.hpp"
#include "engine/shader/reflection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using engine::rhi::DescriptorBindingDesc;
using engine::rhi::DescriptorSetLayoutDesc;
using engine::rhi::DescriptorType;
using engine::rhi::ShaderStage;
using engine::shader::BindingClass;
using engine::shader::compare_reflections;
using engine::shader::CpuBlockLayout;
using engine::shader::Diagnostic;
using engine::shader::parse_dxil_listing;
using engine::shader::parse_spirv_listing;
using engine::shader::ReflectedMember;
using engine::shader::ShaderReflection;
using engine::shader::validate_bindings;
using engine::shader::validate_cpu_layouts;

namespace {

// dx12_compute.hlsl as dxc -Fc prints it: a cbuffer at b0, a byte-address SRV at t1, and a byte-address UAV at u2.
constexpr const char* kDxilCompute = R"(; Buffer Definitions:
;
; cbuffer Params
; {
;
;   struct Params
;   {
;
;       float Scale;                                  ; Offset:    0
;       float Offset;                                 ; Offset:    4
;       uint Count;                                   ; Offset:    8
;       uint Pad;                                     ; Offset:   12
;
;   } Params;                                         ; Offset:    0 Size:    16
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; Params                            cbuffer      NA          NA     CB0            cb0     1
; Input                             texture    byte         r/o      T0             t1     1
; Output                                UAV    byte         r/w      U0             u2     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
)";

// The same shader as SPIR-V (dxc -spirv -fvk-use-dx-layout -Fc). Offsets match the DXIL listing; names come from OpName.
constexpr const char* kSpirvCompute = R"(; SPIR-V
; Version: 1.5
               OpCapability Shader
               OpEntryPoint GLCompute %main "main" %gl_GlobalInvocationID %Params %Input %Output
               OpName %type_Params "type.Params"
               OpMemberName %type_Params 0 "Scale"
               OpMemberName %type_Params 1 "Offset"
               OpMemberName %type_Params 2 "Count"
               OpMemberName %type_Params 3 "Pad"
               OpName %Params "Params"
               OpName %Input "Input"
               OpName %Output "Output"
               OpDecorate %Params DescriptorSet 0
               OpDecorate %Params Binding 0
               OpDecorate %Input DescriptorSet 0
               OpDecorate %Input Binding 1
               OpDecorate %Output DescriptorSet 0
               OpDecorate %Output Binding 2
               OpMemberDecorate %type_Params 0 Offset 0
               OpMemberDecorate %type_Params 1 Offset 4
               OpMemberDecorate %type_Params 2 Offset 8
               OpMemberDecorate %type_Params 3 Offset 12
               OpDecorate %type_Params Block
               OpDecorate %_runtimearr_uint ArrayStride 4
               OpMemberDecorate %type_ByteAddressBuffer 0 Offset 0
               OpMemberDecorate %type_ByteAddressBuffer 0 NonWritable
               OpDecorate %type_ByteAddressBuffer Block
               OpMemberDecorate %type_RWByteAddressBuffer 0 Offset 0
               OpDecorate %type_RWByteAddressBuffer Block
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
       %uint = OpTypeInt 32 0
      %float = OpTypeFloat 32
%type_Params = OpTypeStruct %float %float %uint %uint
%_ptr_Uniform_type_Params = OpTypePointer Uniform %type_Params
%_runtimearr_uint = OpTypeRuntimeArray %uint
%type_ByteAddressBuffer = OpTypeStruct %_runtimearr_uint
%_ptr_StorageBuffer_type_ByteAddressBuffer = OpTypePointer StorageBuffer %type_ByteAddressBuffer
%type_RWByteAddressBuffer = OpTypeStruct %_runtimearr_uint
%_ptr_StorageBuffer_type_RWByteAddressBuffer = OpTypePointer StorageBuffer %type_RWByteAddressBuffer
     %Params = OpVariable %_ptr_Uniform_type_Params Uniform
      %Input = OpVariable %_ptr_StorageBuffer_type_ByteAddressBuffer StorageBuffer
     %Output = OpVariable %_ptr_StorageBuffer_type_RWByteAddressBuffer StorageBuffer
)";

// Frame at space 1, b3; Albedo at space 1, t4; Samp at s0; Out at u1. Shows a register space and a non-zero space.
constexpr const char* kDxilSpaces = R"(; Buffer Definitions:
;
; cbuffer Frame
; {
;
;   struct Frame
;   {
;
;       float4 Tint;                                  ; Offset:    0
;
;   } Frame;                                          ; Offset:    0 Size:    16
;
; }
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
; Frame                             cbuffer      NA          NA     CB0     cb3,space1     1
; Samp                              sampler      NA          NA      S0             s0     1
; Albedo                            texture     f32          2d      T0      t4,space1     1
; Out                                   UAV     f32          2d      U0             u1     1
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
)";

// Sampled images: Albedo is read through SampledImage (Sampled = 1); Out is a storage image (Sampled = 2) at set 0, binding 1.
constexpr const char* kSpirvSpaces = R"(; SPIR-V
               OpName %Frame "Frame"
               OpMemberName %type_Frame 0 "Tint"
               OpName %Albedo "Albedo"
               OpName %Out "Out"
               OpName %Samp "Samp"
               OpDecorate %Frame DescriptorSet 1
               OpDecorate %Frame Binding 3
               OpDecorate %Albedo DescriptorSet 1
               OpDecorate %Albedo Binding 4
               OpDecorate %Out DescriptorSet 0
               OpDecorate %Out Binding 1
               OpDecorate %Samp DescriptorSet 0
               OpDecorate %Samp Binding 0
               OpMemberDecorate %type_Frame 0 Offset 0
               OpDecorate %type_Frame Block
     %float = OpTypeFloat 32
   %v4float = OpTypeVector %float 4
%type_Frame = OpTypeStruct %v4float
%_ptr_Uniform_type_Frame = OpTypePointer Uniform %type_Frame
%type_2d_image = OpTypeImage %float 2D 2 0 0 1 Unknown
%_ptr_UniformConstant_type_2d_image = OpTypePointer UniformConstant %type_2d_image
%type_2d_image_0 = OpTypeImage %float 2D 2 0 0 2 Rgba32f
%_ptr_UniformConstant_type_2d_image_0 = OpTypePointer UniformConstant %type_2d_image_0
%type_sampler = OpTypeSampler
%_ptr_UniformConstant_type_sampler = OpTypePointer UniformConstant %type_sampler
     %Frame = OpVariable %_ptr_Uniform_type_Frame Uniform
    %Albedo = OpVariable %_ptr_UniformConstant_type_2d_image UniformConstant
       %Out = OpVariable %_ptr_UniformConstant_type_2d_image_0 UniformConstant
      %Samp = OpVariable %_ptr_UniformConstant_type_sampler UniformConstant
)";

std::vector<DescriptorBindingDesc> compute_set() {
    return {
        DescriptorBindingDesc{0, DescriptorType::ConstantBuffer, 1, ShaderStage::Compute},
        DescriptorBindingDesc{1, DescriptorType::ShaderResource, 1, ShaderStage::Compute},
        DescriptorBindingDesc{2, DescriptorType::UnorderedAccess, 1, ShaderStage::Compute},
    };
}

std::vector<CpuBlockLayout> compute_cpu_layout() {
    CpuBlockLayout params;
    params.name = "Params";
    params.set = 0;
    params.binding = 0;
    params.size_bytes = 16;
    params.members = {ReflectedMember{"Scale", 0}, ReflectedMember{"Offset", 4}, ReflectedMember{"Count", 8}, ReflectedMember{"Pad", 12}};
    return {params};
}

ShaderReflection must_parse(const engine::core::Result<ShaderReflection, std::string>& result) {
    if (!result.ok()) {
        FAIL(result.error());
    }
    return result.value();
}

}  // namespace

TEST_CASE("The DXIL listing gives each binding's class, space, register, and count", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_dxil_listing(kDxilCompute));
    REQUIRE(reflection.bindings.size() == 3);
    CHECK(reflection.bindings[0].cls == BindingClass::ConstantBuffer);
    CHECK(reflection.bindings[0].set == 0);
    CHECK(reflection.bindings[0].binding == 0);
    CHECK(reflection.bindings[1].cls == BindingClass::ShaderResource);
    CHECK(reflection.bindings[1].binding == 1);
    CHECK(reflection.bindings[2].cls == BindingClass::UnorderedAccess);
    CHECK(reflection.bindings[2].binding == 2);
}

TEST_CASE("The DXIL listing gives constant buffer members their offsets and the block its size", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_dxil_listing(kDxilCompute));
    REQUIRE(reflection.blocks.size() == 1);
    CHECK(reflection.blocks[0].name == "Params");
    CHECK(reflection.blocks[0].size_bytes == 16);
    REQUIRE(reflection.blocks[0].members.size() == 4);
    CHECK(reflection.blocks[0].members[1].name == "Offset");
    CHECK(reflection.blocks[0].members[1].offset == 4);
    CHECK(reflection.blocks[0].members[3].offset == 12);
}

TEST_CASE("A DXIL register space is read from the bind column, and a sampler is classed as one", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_dxil_listing(kDxilSpaces));
    REQUIRE(reflection.bindings.size() == 4);
    bool saw_frame = false;
    for (const auto& binding : reflection.bindings) {
        if (binding.name == "Frame") {
            saw_frame = true;
            CHECK(binding.set == 1);
            CHECK(binding.binding == 3);
        }
        if (binding.name == "Samp") {
            CHECK(binding.cls == BindingClass::Sampler);
            CHECK(binding.set == 0);
        }
    }
    CHECK(saw_frame);
}

TEST_CASE("The SPIR-V listing gives the same bindings and offsets as the DXIL listing", "[unit]") {
    const ShaderReflection dxil = must_parse(parse_dxil_listing(kDxilCompute));
    const ShaderReflection spirv = must_parse(parse_spirv_listing(kSpirvCompute));
    CHECK(compare_reflections("dx12_compute.hlsl", dxil, spirv).empty());
    REQUIRE(spirv.blocks.size() == 1);
    CHECK(spirv.blocks[0].name == "Params");
    CHECK(spirv.blocks[0].members[2].offset == 8);
}

TEST_CASE("The SPIR-V listing tells a sampled image from a storage image and reads a sampler", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_spirv_listing(kSpirvSpaces));
    REQUIRE(reflection.bindings.size() == 4);
    for (const auto& binding : reflection.bindings) {
        if (binding.name == "Albedo") {
            CHECK(binding.cls == BindingClass::ShaderResource);
            CHECK(binding.set == 1);
            CHECK(binding.binding == 4);
        } else if (binding.name == "Out") {
            CHECK(binding.cls == BindingClass::UnorderedAccess);
        } else if (binding.name == "Samp") {
            CHECK(binding.cls == BindingClass::Sampler);
        }
    }
}

TEST_CASE("A listing with an unknown DXIL resource type is rejected, not guessed", "[unit]") {
    const std::string listing =
        "; Resource Bindings:\n;\n; Name Type Format Dim ID HLSL Bind Count\n"
        "; Odd raytracingaccel NA NA T0 t0 1\n;\n";
    CHECK_FALSE(parse_dxil_listing(listing).ok());
}

TEST_CASE("Bindings that match the declared layout validate with no diagnostics", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_dxil_listing(kDxilCompute));
    const std::vector<DescriptorSetLayoutDesc> layouts = {DescriptorSetLayoutDesc{compute_set()}};
    CHECK(validate_bindings("dx12_compute.hlsl", reflection, layouts).empty());
    CHECK(validate_cpu_layouts("dx12_compute.hlsl", reflection, compute_cpu_layout()).empty());
}

TEST_CASE("A binding whose type, count, set, or number differs from the layout is reported at its place", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_dxil_listing(kDxilCompute));

    // The UAV is declared as a read-only SRV: the type disagrees.
    auto wrong_type = compute_set();
    wrong_type[2].type = DescriptorType::ShaderResource;
    std::vector<Diagnostic> diagnostics = validate_bindings("dx12_compute.hlsl", reflection, {DescriptorSetLayoutDesc{wrong_type}});
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].file == "dx12_compute.hlsl");
    CHECK(diagnostics[0].message.find("unordered access") != std::string::npos);

    // The SRV is declared as an array of four: the count disagrees.
    auto wrong_count = compute_set();
    wrong_count[1].count = 4;
    diagnostics = validate_bindings("dx12_compute.hlsl", reflection, {DescriptorSetLayoutDesc{wrong_count}});
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].message.find("declares 4") != std::string::npos);

    // Set 0 is missing entirely.
    diagnostics = validate_bindings("dx12_compute.hlsl", reflection, {});
    CHECK(diagnostics.size() == 3);

    // Binding 2 is absent from the set.
    auto missing = compute_set();
    missing.pop_back();
    diagnostics = validate_bindings("dx12_compute.hlsl", reflection, {DescriptorSetLayoutDesc{missing}});
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].message.find("does not declare it") != std::string::npos);
}

TEST_CASE("A sampler in the shader is reported as outside the descriptor model", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_dxil_listing(kDxilSpaces));
    const auto diagnostics = validate_bindings("spaces.hlsl", reflection, {});
    bool saw_sampler = false;
    for (const Diagnostic& diagnostic : diagnostics) {
        saw_sampler = saw_sampler || diagnostic.message.find("samplers are not in the descriptor model") != std::string::npos;
    }
    CHECK(saw_sampler);
}

TEST_CASE("A CPU struct whose offsets or size differ from the shader is rejected member by member", "[unit]") {
    const ShaderReflection reflection = must_parse(parse_dxil_listing(kDxilCompute));

    auto shifted = compute_cpu_layout();
    shifted[0].members[1].offset = 8;  // CPU puts Offset where the shader has Count
    auto diagnostics = validate_cpu_layouts("dx12_compute.hlsl", reflection, shifted);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].message.find("Params.Offset is at offset 4 in the shader but 8 on the CPU") != std::string::npos);

    auto resized = compute_cpu_layout();
    resized[0].size_bytes = 32;
    diagnostics = validate_cpu_layouts("dx12_compute.hlsl", reflection, resized);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].message.find("16 bytes in the shader but 32") != std::string::npos);

    auto extra = compute_cpu_layout();
    extra[0].members.push_back(ReflectedMember{"Tint", 16});
    diagnostics = validate_cpu_layouts("dx12_compute.hlsl", reflection, extra);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].message.find("does not exist in the shader") != std::string::npos);

    auto missing = compute_cpu_layout();
    missing[0].members.pop_back();
    diagnostics = validate_cpu_layouts("dx12_compute.hlsl", reflection, missing);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].message.find("Params.Pad is missing from the CPU struct") != std::string::npos);
}

TEST_CASE("Reflections of one shader that disagree on a binding are reported by both targets' names", "[unit]") {
    const ShaderReflection dxil = must_parse(parse_dxil_listing(kDxilCompute));
    ShaderReflection spirv = must_parse(parse_spirv_listing(kSpirvCompute));
    spirv.bindings[1].count = 2;
    const auto diagnostics = compare_reflections("dx12_compute.hlsl", dxil, spirv);
    CHECK_FALSE(diagnostics.empty());
    CHECK(diagnostics[0].message.find("dxil and spirv disagree") != std::string::npos);
}
