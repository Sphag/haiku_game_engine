#pragma once

// The pixels of one rendered frame, read back from the GPU or produced by the reference rasterizer. Both paths fill the same layout,
// so one golden fixture checks both.

#include "engine/render/frame_snapshot.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace engine::render {

struct SceneImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::array<std::uint8_t, 4>> color;  // RGBA8, row-major from the top-left corner
    std::vector<std::uint32_t> pick_ids;             // SnapshotItem::pick_id per pixel, zero for background
    std::vector<float> depth;                        // NDC depth of the nearest surface, zero for background
    // Motion vectors (US-036): the change in the surface's screen position since the previous frame, in UV units (x right, y down), so
    // the value is the same on both backends. Zero for background and for an unmoved surface.
    std::vector<glm::vec2> velocity;

    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const noexcept {
        return static_cast<std::size_t>(y) * width + x;
    }
};

// Picking: the entity under a pixel. Empty for background and for a pixel outside the image.
[[nodiscard]] std::optional<ecs::EntityHandle> pick_entity(const SceneImage& image, const FrameSnapshot& snapshot, std::uint32_t x,
                                                           std::uint32_t y);

}  // namespace engine::render
