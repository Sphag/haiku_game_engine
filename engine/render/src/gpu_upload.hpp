#pragma once

// Uploads for the ray-traced passes (US-043, US-044). A device buffer that the shaders read is filled by a copy through the render graph,
// so the barriers and the wait are the graph's. The ray-traced passes call these between frames.

#include "engine/rhi/device.hpp"
#include "engine/rhi/types.hpp"

#include <cstdint>
#include <vector>

namespace engine::render::detail {

// A description for a buffer of `size` bytes with `usage` (the name is for diagnostics).
[[nodiscard]] rhi::BufferDesc gpu_buffer_desc(std::uint64_t size, rhi::BufferUsage usage, const char* name);

// Copies `bytes` into `buffer`, which has `usage` and sits in `before`, and leaves it in `after` (ShaderRead by default; an index buffer
// passes IndexBuffer). The copy must be non-empty and fit the buffer. The staging buffer is destroyed once the graph has waited.
[[nodiscard]] rhi::RhiStatus upload_buffer_through_graph(rhi::Device& device, rhi::BufferHandle buffer, rhi::BufferUsage usage,
                                                         std::uint64_t size, const std::vector<std::uint8_t>& bytes, rhi::ResourceState before,
                                                         rhi::ResourceState after = rhi::ResourceState::ShaderRead);

}  // namespace engine::render::detail
