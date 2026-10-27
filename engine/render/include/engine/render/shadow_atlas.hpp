#pragma once

// Shadow atlas (US-035). One square depth texture holds many shadow maps as square tiles. A tile is a region of the atlas that one light (or
// one cascade) renders into. The tiles persist across frames under a caller-chosen id, so a tile is re-rendered only when its content
// changes: its key (a hash of the light's matrix, tuning, and the casters inside its frustum) changes, or it is invalidated.
//
// The GPU draws into a tile by remapping clip space (tile_remap), and the shader reads it by the same mapping (docs/lighting.md). Tiles
// are allocated by shelves: a shelf is a row of tiles of one height, and a freed tile is reused for a request of the same size.

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace engine::render {

// A square region of the atlas, in texels, with its origin at the top left.
struct AtlasRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t size = 0;
};

struct ShadowTileRequest {
    bool granted = false;  // false when the atlas has no room; the light then casts no shadow this frame
    AtlasRect rect;
    bool render = false;   // the tile's content must be drawn this frame
};

class ShadowAtlas {
public:
    explicit ShadowAtlas(std::uint32_t size);

    [[nodiscard]] std::uint32_t size() const noexcept {
        return size_;
    }
    // Number of tiles that hold an allocation, granted this frame or not yet released.
    [[nodiscard]] std::size_t tile_count() const noexcept {
        return entries_.size();
    }

    // Every tile becomes unrequested. Call it once at the start of a frame, before any request.
    void begin_frame();
    // Grants the tile for `id` at `tile_size`, allocating it on the first request or when its size changes. `key` identifies the content;
    // the tile renders when the key differs from the last one, when it was just allocated, or when it was invalidated. A request that the
    // atlas cannot fit is not granted.
    [[nodiscard]] ShadowTileRequest request(std::uint32_t id, std::uint32_t tile_size, std::uint64_t key);
    // Marks a tile's content as stale. Its next request renders it.
    void invalidate(std::uint32_t id);
    void invalidate_all();
    // Releases the tiles that were not requested this frame, so their room can serve other lights.
    void end_frame();

private:
    struct Shelf {
        std::uint32_t y = 0;
        std::uint32_t height = 0;
        std::uint32_t used = 0;  // texels of the row taken so far
    };
    struct Entry {
        AtlasRect rect;
        std::uint64_t key = 0;
        bool valid = false;
        bool requested = false;
    };

    [[nodiscard]] bool allocate(std::uint32_t tile_size, AtlasRect& rect);

    std::uint32_t size_ = 0;
    std::uint32_t next_row_ = 0;  // top of the next unused shelf
    std::vector<Shelf> shelves_;
    std::vector<AtlasRect> free_;
    std::map<std::uint32_t, Entry> entries_;
};

// Clip-space matrix (engine convention, before any Vulkan flip) that maps a light's clip into `rect`: its clip x and y are scaled into
// the tile and offset to it. z and w are unchanged, so depth values stay the same. vulkan_clip selects the row order of the backend
// (docs/runtime-conventions.md): DX12 rows run down from +1, Vulkan rows run up from -1 after the flip.
[[nodiscard]] glm::mat4 tile_remap(const AtlasRect& rect, std::uint32_t atlas_size, bool vulkan_clip) noexcept;
// The sign that maps a backend NDC y to a row inside a tile: row = (0.5 + 0.5 * sign * y) * size. The shader uses the same value.
[[nodiscard]] float tile_row_sign(bool vulkan_clip) noexcept;

// FNV-1a over raw bytes, chained through `seed`. Used to build tile keys from matrices and parameters.
[[nodiscard]] std::uint64_t hash_bytes(const void* data, std::size_t size, std::uint64_t seed) noexcept;

}  // namespace engine::render
