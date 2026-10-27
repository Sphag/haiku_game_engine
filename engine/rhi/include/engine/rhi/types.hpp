#pragma once

#include "engine/rhi/handle.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::rhi {

enum class Backend : std::uint8_t {
    Validation,  // CPU reference device in this module; enforces the contract without a GPU
    Dx12,        // Windows DirectX 12 (US-027)
    Vulkan,      // Windows and Linux Vulkan (US-028)
};
[[nodiscard]] std::string_view to_string(Backend backend) noexcept;

enum class QueueType : std::uint8_t {
    Graphics,
    Compute,
    Copy,
};
[[nodiscard]] std::string_view to_string(QueueType queue) noexcept;

// Bitwise operators for flag enums. A type opts in by specializing EnableFlagOperators.
template <typename E>
struct EnableFlagOperators : std::false_type {};

// Spelled as enable_if rather than a concept: the CI's GCC 9 has no concepts.
template <typename E>
using EnableIfFlagEnum = std::enable_if_t<std::is_enum_v<E> && EnableFlagOperators<E>::value, int>;

template <typename E, EnableIfFlagEnum<E> = 0>
[[nodiscard]] constexpr E operator|(E lhs, E rhs) noexcept {
    using Underlying = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<Underlying>(lhs) | static_cast<Underlying>(rhs));
}

template <typename E, EnableIfFlagEnum<E> = 0>
[[nodiscard]] constexpr E operator&(E lhs, E rhs) noexcept {
    using Underlying = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<Underlying>(lhs) & static_cast<Underlying>(rhs));
}

// True when every bit in `flags` is set in `value`.
template <typename E, EnableIfFlagEnum<E> = 0>
[[nodiscard]] constexpr bool has_flags(E value, E flags) noexcept {
    return (value & flags) == flags;
}

enum class Format : std::uint8_t {
    Unknown,
    R8G8B8A8Unorm,
    R8G8B8A8Srgb,
    B8G8R8A8Unorm,
    R16G16B16A16Float,
    R32Float,
    R32Uint,
    D32Float,
    R32G32B32Float,  // vertex positions and normals only; not a color or depth target
    R32G32Float,     // vertex texture coordinates, and the velocity color target (US-036)
    R32G32B32A32Float,  // vertex blend weights only; not a color or depth target (US-036)
};
[[nodiscard]] constexpr bool is_depth_format(Format format) noexcept {
    return format == Format::D32Float;
}
// Bytes per texel. Zero for Unknown and depth formats, which texture-to-buffer copies do not support.
[[nodiscard]] constexpr std::uint32_t bytes_per_pixel(Format format) noexcept {
    switch (format) {
        case Format::R8G8B8A8Unorm:
        case Format::R8G8B8A8Srgb:
        case Format::B8G8R8A8Unorm:
        case Format::R32Float:
        case Format::R32Uint:
            return 4;
        case Format::R16G16B16A16Float:
            return 8;
        case Format::R32G32B32Float:
            return 12;
        case Format::R32G32Float:
            return 8;
        case Format::R32G32B32A32Float:
            return 16;
        case Format::Unknown:
        case Format::D32Float:
            return 0;
    }
    return 0;
}

// Required alignment of a texture-to-buffer copy's row pitch and of its destination offset. Both are the D3D12 limits, and
// the Vulkan path honors them too, so one contract covers both backends.
inline constexpr std::uint64_t kCopyRowPitchAlignment = 256;
inline constexpr std::uint64_t kTexturePlacementAlignment = 512;

// Bytes per row in a buffer that a texture copy writes: the tight row size rounded up to kCopyRowPitchAlignment.
[[nodiscard]] constexpr std::uint64_t copy_row_pitch(std::uint32_t width, Format format) noexcept {
    const std::uint64_t tight = static_cast<std::uint64_t>(width) * bytes_per_pixel(format);
    return (tight + kCopyRowPitchAlignment - 1) / kCopyRowPitchAlignment * kCopyRowPitchAlignment;
}

enum class BufferUsage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Constant = 1u << 2,
    ShaderResource = 1u << 3,
    UnorderedAccess = 1u << 4,
    IndirectArgument = 1u << 5,
    CopySource = 1u << 6,
    CopyDestination = 1u << 7,
    // CPU-readable readback memory. Must be combined with CopyDestination; read_buffer() reads it back after the GPU
    // finishes. Such a buffer starts in CopyDestination and allows no other state.
    CpuRead = 1u << 8,
    // CPU-writable upload memory. Must be combined with CopySource; upload_buffer() fills it. Such a buffer starts in
    // CopySource and allows no other state, because upload memory is never transitioned.
    CpuWrite = 1u << 9,
};
template <>
struct EnableFlagOperators<BufferUsage> : std::true_type {};

enum class TextureUsage : std::uint32_t {
    None = 0,
    ShaderResource = 1u << 0,
    UnorderedAccess = 1u << 1,
    RenderTarget = 1u << 2,
    DepthStencil = 1u << 3,
    CopySource = 1u << 4,
    CopyDestination = 1u << 5,
};
template <>
struct EnableFlagOperators<TextureUsage> : std::true_type {};

// Amplification is the task stage of a mesh pipeline (DX12 amplification shader, Vulkan task shader). It is optional: a mesh pipeline
// without it dispatches its mesh groups directly.
// RayTracing (US-042) is a visibility only: the ray generation, miss, and hit shaders of a ray tracing pipeline. A binding that names it
// is visible to those shaders. It is never a pipeline stage, so a graphics or compute pipeline rejects it.
enum class ShaderStage : std::uint32_t {
    None = 0,
    Vertex = 1u << 0,
    Pixel = 1u << 1,
    Compute = 1u << 2,
    Mesh = 1u << 3,
    Amplification = 1u << 4,
    RayTracing = 1u << 5,
};
template <>
struct EnableFlagOperators<ShaderStage> : std::true_type {};

// Every resource carries one tracked state. A transition names the state it expects (`before`) and the state it
// produces (`after`); the device rejects a transition whose `before` does not match the tracked state. A resource starts in
// Common, except a CpuRead buffer, which starts in CopyDestination. Each state is valid only for the usage flags that allow
// it. A back buffer starts in Present.
enum class ResourceState : std::uint8_t {
    Common,
    VertexBuffer,
    IndexBuffer,
    ConstantBuffer,
    ShaderRead,
    UnorderedAccess,
    RenderTarget,
    DepthWrite,
    DepthRead,
    IndirectArgument,
    CopySource,
    CopyDestination,
    Present,
};

// Whether a usage flag set allows a state. Shared by every backend and by the CPU reference device, so the rules
// cannot drift. A CpuRead buffer allows only CopyDestination.
[[nodiscard]] bool buffer_state_allowed(BufferUsage usage, ResourceState state) noexcept;
[[nodiscard]] bool texture_state_allowed(TextureUsage usage, ResourceState state) noexcept;

enum class IndexFormat : std::uint8_t {
    Uint16,
    Uint32,
};
[[nodiscard]] constexpr std::uint64_t index_stride_bytes(IndexFormat format) noexcept {
    return format == IndexFormat::Uint16 ? 2u : 4u;
}

enum class DescriptorType : std::uint8_t {
    ConstantBuffer,
    ShaderResource,
    UnorderedAccess,
};

enum class PipelineKind : std::uint8_t {
    Graphics,  // vertex + pixel, drawn with indexed draws
    Compute,   // compute
    Mesh,      // [amplification +] mesh + pixel, dispatched with dispatch_mesh (requires the MeshShaders feature)
};

inline constexpr std::uint32_t kMaxColorTargets = 8;
inline constexpr std::uint32_t kMaxDescriptorSetsPerPipeline = 4;
inline constexpr std::uint32_t kMaxVertexBufferSlots = 8;
inline constexpr std::uint32_t kMaxQueryCount = 4096;
// Layout of one DrawIndexedIndirect record: indexCount, instanceCount, firstIndex, baseVertex, firstInstance.
inline constexpr std::uint64_t kDrawIndexedIndirectStride = 20;
// Layout of the count written by an indirect count draw.
inline constexpr std::uint64_t kIndirectCountSize = 4;

struct BufferDesc {
    std::uint64_t size_bytes = 0;
    BufferUsage usage = BufferUsage::None;
    std::string debug_name;
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_levels = 1;
    Format format = Format::Unknown;
    TextureUsage usage = TextureUsage::None;
    std::string debug_name;
};

struct DescriptorBindingDesc {
    std::uint32_t binding = 0;
    DescriptorType type = DescriptorType::ConstantBuffer;
    std::uint32_t count = 1;  // array length; 1 for a single descriptor
    ShaderStage visibility = ShaderStage::None;

    friend bool operator==(const DescriptorBindingDesc& lhs, const DescriptorBindingDesc& rhs) noexcept {
        return lhs.binding == rhs.binding && lhs.type == rhs.type && lhs.count == rhs.count && lhs.visibility == rhs.visibility;
    }
    friend bool operator!=(const DescriptorBindingDesc& lhs, const DescriptorBindingDesc& rhs) noexcept {
        return !(lhs == rhs);
    }
};

struct DescriptorSetLayoutDesc {
    std::vector<DescriptorBindingDesc> bindings;
};

// One compiled stage. Bytecode is DXIL for DX12 and SPIR-V for Vulkan (US-003 produces both from HLSL).
struct ShaderBinary {
    ShaderStage stage = ShaderStage::None;
    std::string entry_point;
    std::vector<std::uint8_t> bytecode;
};

// One vertex attribute of the vertex buffer bound at slot 0. Vulkan reads `location` as layout(location). DX12 matches it to the
// HLSL semantic TEXCOORD<location>, so one shader source works on both backends.
struct VertexAttribute {
    std::uint32_t location = 0;
    Format format = Format::Unknown;
    std::uint32_t offset = 0;  // bytes from the start of the vertex
};

enum class CullMode : std::uint8_t {
    None,  // both faces are drawn (two-sided materials)
    Back,  // back faces are culled
};

// Graphics pipeline state. Graphics pipelines draw triangle lists, write all color channels, and depth-test with LESS when depth_format is
// set. Front faces are counter-clockwise as the viewer sees them on screen (the glTF convention) on both backends. The cull mode is set per
// pipeline. alpha_blend is off unless asked for; when on, every color target blends with
// color = src * src.a + dst * (1 - src.a) and alpha = src.a + dst.a * (1 - src.a). additive_blend (US-051) is the other blend: color = src + dst
// and alpha = dst.a, so a particle adds light and keeps the destination alpha. The two are exclusive. depth_write decides whether a passing fragment
// writes depth; the test still runs. begin_render_pass sets the viewport and scissor to the smallest target, with depth 0 to 1. NDC x is
// the same on both backends. NDC y is up on DX12 and down on Vulkan, so a Vulkan shader consumes coords::to_vulkan_clip() of an engine
// projection (docs/runtime-conventions.md).
struct PipelineDesc {
    PipelineKind kind = PipelineKind::Graphics;
    std::vector<ShaderBinary> shaders;
    std::vector<DescriptorSetLayoutHandle> set_layouts;  // set index = position in this list
    std::vector<Format> color_formats;                   // must match the render pass it is drawn in
    Format depth_format = Format::Unknown;               // Unknown when the pipeline has no depth target
    CullMode cull_mode = CullMode::None;
    bool alpha_blend = false;     // needs blendable color formats (pipeline_rules.hpp)
    bool additive_blend = false;  // needs blendable color formats and no alpha_blend (pipeline_rules.hpp)
    bool depth_write = true;   // only meaningful with a depth format
    // Vertex input for slot 0. Empty for pipelines without vertex input (compute, and the full-screen fixtures).
    std::uint32_t vertex_stride = 0;
    std::vector<VertexAttribute> vertex_attributes;
    // Per-instance input for slot 1 (US-037). Its elements advance once per instance, and an instance's element is selected by the
    // draw's first_instance on both backends (plus the instance index inside the draw). Empty when the pipeline reads none. An indirect
    // draw uses it to tell each drawn item its index; SV_InstanceID alone would not on DX12.
    std::uint32_t instance_stride = 0;
    std::vector<VertexAttribute> instance_attributes;
    std::string debug_name;
};

struct RenderPassDesc {
    std::vector<TextureHandle> color_targets;  // each in RenderTarget state
    std::optional<TextureHandle> depth_target;  // in DepthWrite state when present
    // When set, every color target is cleared to this RGBA value when the pass begins. A zero value clears an integer target to 0.
    std::optional<std::array<float, 4>> clear_color;
    // When set, the depth target is cleared to this value when the pass begins. Needs depth_target. Unset keeps the depth contents.
    std::optional<float> clear_depth;
};

// Ray tracing (US-041). Triangle geometry only: float3 positions read from a vertex buffer, with an optional index buffer. Both buffers
// need ShaderResource usage, and a build needs them in ShaderRead state (DXR reads geometry as shader input). A structure with
// allow_refit can be updated in place by a later Refit build, which is how deforming or skinned geometry is tracked.
struct BottomLevelDesc {
    BufferHandle vertex_buffer;                // Vertex and ShaderResource usage
    std::uint32_t vertex_stride = 0;           // bytes per vertex; a multiple of four
    std::uint32_t position_offset = 0;         // byte offset of the float3 position inside a vertex
    std::uint32_t vertex_count = 0;
    BufferHandle index_buffer;                 // Index and ShaderResource usage; an invalid handle makes the geometry non-indexed
    IndexFormat index_format = IndexFormat::Uint32;
    std::uint32_t index_count = 0;             // indexed geometry only; a multiple of three
    bool opaque = true;                        // opaque geometry skips any-hit evaluation
    bool allow_refit = false;
    std::string debug_name;
};

// Top-level structure: up to max_instances references to bottom-level structures, written with Device::write_top_level.
struct TopLevelDesc {
    std::uint32_t max_instances = 0;
    bool allow_refit = false;
    std::string debug_name;
};

// One instance in a top-level structure. The transform is a row-major 3x4 object-to-world matrix, so the first row is
// (m00, m01, m02, m03) and the translation sits in the last column.
struct TopLevelInstance {
    AccelerationStructureHandle bottom_level;
    std::array<float, 12> transform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::uint32_t instance_id = 0;  // 24 bits, readable as InstanceID() in shaders
    std::uint8_t mask = 0xFF;       // ray mask, tested against TraceRay's instance mask
};

// Whether a build starts a structure from its geometry (Build) or updates the structure from the same geometry (Refit). A refit needs
// allow_refit and an earlier build, and a top-level refit keeps the instance count of that build.
enum class BuildMode : std::uint8_t {
    Build,
    Refit,
};

// Ray tracing pipeline (US-041, US-042): one shader library with a ray generation, a miss, and a closest-hit entry. DX12 takes the DXIL
// (lib_6_3) and Vulkan takes the SPIR-V that the same source compiles to (lib_6_3 with -spirv). The hit group has triangles and a
// closest-hit shader only. Set i is bound with set_descriptor_set(i), and the library reads it through register space i, as compute
// pipelines do. The stage of `library` is not used.
struct RayTracingPipelineDesc {
    ShaderBinary library;
    std::string raygen_entry;
    std::string miss_entry;
    std::string closest_hit_entry;
    std::vector<DescriptorSetLayoutHandle> set_layouts;
    std::uint32_t max_payload_bytes = 16;   // multiple of four, at most 128
    std::uint32_t max_attribute_bytes = 8;  // 2 to 32
    std::uint32_t max_recursion_depth = 1;  // 1 to 31
    std::string debug_name;
};

}  // namespace engine::rhi
