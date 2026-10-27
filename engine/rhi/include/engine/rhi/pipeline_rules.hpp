#pragma once

// Checks shared by every backend and by the CPU reference device, so the rules cannot drift between them.

#include "engine/rhi/error.hpp"
#include "engine/rhi/types.hpp"

namespace engine::rhi {

// Checks the vertex input of a pipeline description: only graphics pipelines have vertex input, the stride holds every attribute,
// each attribute has a format the contract supports (R32G32B32Float, R32G32Float, R32G32B32A32Float, or R32Uint), and locations are unique. A description without
// vertex input always passes.
[[nodiscard]] RhiStatus check_vertex_layout(const PipelineDesc& desc);

// Formats that a color target may blend into. Integer targets (entity IDs) and R32Float do not blend on every device, so the contract
// keeps them out of alpha-blended pipelines.
[[nodiscard]] constexpr bool is_blendable(Format format) noexcept {
    return format == Format::R8G8B8A8Unorm || format == Format::R8G8B8A8Srgb || format == Format::B8G8R8A8Unorm ||
           format == Format::R16G16B16A16Float;
}

// Checks the blend state of a pipeline description: a pipeline with alpha_blend or additive_blend needs graphics kind and blendable color
// formats, and the two blends are exclusive. A description with neither always passes.
[[nodiscard]] RhiStatus check_blend_state(const PipelineDesc& desc);

}  // namespace engine::rhi
