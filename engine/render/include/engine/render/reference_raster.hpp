#pragma once

// CPU reference rasterizer for a frame snapshot (US-033). It needs no GPU, so the golden fixtures run on every machine, and the GPU
// results are checked against it. It draws the same items with the same clip matrix and depth rule as the scene shader. Triangles with a
// vertex at or behind the eye (w <= 0) are skipped; the golden scenes keep clear of that case, and the GPU clips it instead.

#include "engine/render/frame_snapshot.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/scene_image.hpp"

#include <cstdint>
#include <vector>

namespace engine::render {

// Background: color (0, 0, 0, 0), pick id 0, depth 0, velocity 0. The depth test starts at 1 (far), as the GPU depth buffer is cleared to 1.
// Skinned items are posed by their palettes (US-036), and their motion vectors use `previous` for the last frame's pose and camera.
[[nodiscard]] SceneImage rasterize_reference(const FrameSnapshot& snapshot, const std::vector<MeshData>& meshes, std::uint32_t width,
                                             std::uint32_t height, const MotionHistory& previous);

// The same, with no history: the frame is its own previous frame, so every motion vector is zero.
[[nodiscard]] SceneImage rasterize_reference(const FrameSnapshot& snapshot, const std::vector<MeshData>& meshes, std::uint32_t width,
                                             std::uint32_t height);

}  // namespace engine::render
