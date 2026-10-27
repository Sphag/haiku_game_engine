#pragma once

// Shader reflection (US-031). Both targets are read from the compiler's own listing (dxc -Fc), so one model describes DXIL and
// SPIR-V. The engine's binding convention is the one the RHI already uses: descriptor set i is register space i (DXIL) or
// DescriptorSet i (SPIR-V), and a binding's register is its Binding number.

#include "engine/core/result.hpp"
#include "engine/rhi/types.hpp"
#include "engine/shader/diagnostics.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::shader {

enum class ShaderTarget : std::uint8_t {
    Dxil,    // DX12
    SpirV,   // Vulkan
};
[[nodiscard]] std::string_view to_string(ShaderTarget target) noexcept;

enum class BindingClass : std::uint8_t {
    ConstantBuffer,
    ShaderResource,
    UnorderedAccess,
    Sampler,
};
[[nodiscard]] std::string_view to_string(BindingClass cls) noexcept;

struct ReflectedBinding {
    std::string name;  // empty when the compiler did not keep a name
    BindingClass cls = BindingClass::ConstantBuffer;
    std::uint32_t set = 0;      // register space (DXIL) or DescriptorSet (SPIR-V)
    std::uint32_t binding = 0;  // register number or Binding
    std::uint32_t count = 1;    // array length; 1 for a single descriptor

    friend bool operator==(const ReflectedBinding& lhs, const ReflectedBinding& rhs) noexcept {
        return lhs.cls == rhs.cls && lhs.set == rhs.set && lhs.binding == rhs.binding && lhs.count == rhs.count;
    }
};

struct ReflectedMember {
    std::string name;
    std::uint32_t offset = 0;  // byte offset inside the constant buffer
};

// A constant buffer's member layout. size_bytes is 0 when the target does not report one (SPIR-V listings carry no block
// size).
struct ReflectedBlock {
    std::string name;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    std::uint32_t size_bytes = 0;
    std::vector<ReflectedMember> members;
};

struct ShaderReflection {
    ShaderTarget target = ShaderTarget::Dxil;
    std::vector<ReflectedBinding> bindings;  // sorted by (set, binding)
    std::vector<ReflectedBlock> blocks;      // one per constant buffer, in binding order
};

// Parse the listing of a DXIL compile (dxc -Fc on a DXIL target). Fails on a resource type the listing does not describe.
[[nodiscard]] core::Result<ShaderReflection, std::string> parse_dxil_listing(std::string_view listing);
// Parse the listing of a SPIR-V compile (dxc -spirv -Fc). Fails on an unbounded array, which the RHI cannot describe.
[[nodiscard]] core::Result<ShaderReflection, std::string> parse_spirv_listing(std::string_view listing);

// Checks every reflected binding against the declared descriptor set layouts. Set i must exist and hold a binding with the
// same number, type, and array length. Declared bindings the shader does not use are allowed.
[[nodiscard]] std::vector<Diagnostic> validate_bindings(const std::string& source, const ShaderReflection& reflection,
                                                       const std::vector<rhi::DescriptorSetLayoutDesc>& layouts);

// A constant buffer as the CPU side declares it: member byte offsets taken from offsetof, and the struct size from sizeof.
struct CpuBlockLayout {
    std::string name;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    std::uint32_t size_bytes = 0;
    std::vector<ReflectedMember> members;
};

// Checks that each declared block matches the reflected block with the same name: the same member names at the same offsets,
// and the same size when the target reports one. Members that exist on only one side are errors.
[[nodiscard]] std::vector<Diagnostic> validate_cpu_layouts(const std::string& source, const ShaderReflection& reflection,
                                                          const std::vector<CpuBlockLayout>& declared);

// Compares two reflections of the same source (one per target). Bindings must match on set, binding, class, and count, and
// blocks must match on member offsets. Block sizes are not compared, because SPIR-V array strides differ from HLSL packing.
[[nodiscard]] std::vector<Diagnostic> compare_reflections(const std::string& source, const ShaderReflection& lhs,
                                                         const ShaderReflection& rhs);

}  // namespace engine::shader
