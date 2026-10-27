#include "engine/render/shadow_atlas.hpp"

#include <algorithm>

namespace engine::render {

ShadowAtlas::ShadowAtlas(std::uint32_t size) : size_(size) {}

void ShadowAtlas::begin_frame() {
    for (auto& [id, entry] : entries_) {
        entry.requested = false;
    }
}

bool ShadowAtlas::allocate(std::uint32_t tile_size, AtlasRect& rect) {
    if (tile_size == 0 || tile_size > size_) {
        return false;
    }
    // A released tile of the same size is reused before any new room is taken.
    const auto reusable = std::find_if(free_.begin(), free_.end(), [tile_size](const AtlasRect& r) { return r.size == tile_size; });
    if (reusable != free_.end()) {
        rect = *reusable;
        free_.erase(reusable);
        return true;
    }
    for (Shelf& shelf : shelves_) {
        if (shelf.height >= tile_size && shelf.used + tile_size <= size_) {
            rect = AtlasRect{shelf.used, shelf.y, tile_size};
            shelf.used += tile_size;
            return true;
        }
    }
    if (next_row_ + tile_size <= size_) {
        shelves_.push_back(Shelf{next_row_, tile_size, tile_size});
        rect = AtlasRect{0, next_row_, tile_size};
        next_row_ += tile_size;
        return true;
    }
    return false;
}

ShadowTileRequest ShadowAtlas::request(std::uint32_t id, std::uint32_t tile_size, std::uint64_t key) {
    auto found = entries_.find(id);
    if (found != entries_.end() && found->second.rect.size != tile_size) {
        // The size changed, so the tile is released and allocated again. Its content is new.
        free_.push_back(found->second.rect);
        entries_.erase(found);
        found = entries_.end();
    }
    if (found == entries_.end()) {
        AtlasRect rect;
        if (!allocate(tile_size, rect)) {
            return ShadowTileRequest{};
        }
        found = entries_.emplace(id, Entry{rect, 0, false, false}).first;
    }
    Entry& entry = found->second;
    entry.requested = true;
    const bool render = !entry.valid || entry.key != key;
    entry.key = key;
    entry.valid = true;
    return ShadowTileRequest{true, entry.rect, render};
}

void ShadowAtlas::invalidate(std::uint32_t id) {
    const auto found = entries_.find(id);
    if (found != entries_.end()) {
        found->second.valid = false;
    }
}

void ShadowAtlas::invalidate_all() {
    for (auto& [id, entry] : entries_) {
        entry.valid = false;
    }
}

void ShadowAtlas::end_frame() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->second.requested) {
            free_.push_back(it->second.rect);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

glm::mat4 tile_remap(const AtlasRect& rect, std::uint32_t atlas_size, bool vulkan_clip) noexcept {
    const float size = static_cast<float>(atlas_size);
    const float scale = static_cast<float>(rect.size) / size;
    // Clip x maps to pixels as (0.5 + 0.5 x) * size, so the tile's center sits at x0 + size / 2 in pixels.
    const float offset_x = (2.0f * static_cast<float>(rect.x) + static_cast<float>(rect.size)) / size - 1.0f;
    const float offset_y = tile_row_sign(vulkan_clip) * ((2.0f * static_cast<float>(rect.y) + static_cast<float>(rect.size)) / size - 1.0f);
    glm::mat4 remap(1.0f);
    remap[0][0] = scale;
    remap[1][1] = scale;
    remap[3][0] = offset_x;
    remap[3][1] = offset_y;
    return remap;
}

float tile_row_sign(bool vulkan_clip) noexcept {
    return vulkan_clip ? 1.0f : -1.0f;
}

std::uint64_t hash_bytes(const void* data, std::size_t size, std::uint64_t seed) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = seed;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}

}  // namespace engine::render
