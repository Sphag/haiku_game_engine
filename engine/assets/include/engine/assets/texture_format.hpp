#pragma once

// The cooked texture payload (US-021): every mip of one 2D texture, with the metadata a runtime needs to upload it. The
// texture importer writes it, and the tests and later stages read it back through read_texture_payload().
//
// Layout, little-endian throughout (the file is an importer payload; the cooked container wraps it as before):
//   magic "OWTX", u32 format version
//   u32 usage (0 color, 1 normal, 2 linear), u32 encoding (0 rgba8, 1 bc7, 2 bc5), u32 srgb (0 or 1)
//   u32 mip count, then per mip: u32 width, u32 height, u32 byte count, the bytes
//
// Mip 0 is the full image; mip i is max(1, ceil(size / 2^i)) on each side, so an odd size keeps its last row and column.
// Encodings: rgba8 is 4 bytes per texel, row-major, top row first. bc7 and bc5 are block-compressed in 4x4 blocks of 16
// bytes each, stored row-major by block; a partial block at the edge is padded by repeating the last row and column. The
// runtime builds the srgb format from `srgb`, so the texel bytes are the same for either choice.
//
// Rules: usage color is sRGB and every other usage is linear; bc5 stores normals only, so it requires the normal usage;
// bc7 holds color or linear data; rgba8 is the explicit fallback for any usage. Reading checks every count against the
// bytes that remain before it allocates, and it recomputes every mip size from the first mip, so a file cannot describe
// a mip that the runtime would misread.

#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

inline constexpr std::string_view kTexturePayloadMagic = "OWTX";
inline constexpr std::uint32_t kTexturePayloadVersion = 1;

// Largest width or height accepted at import and read time. The limit bounds the decoded image in memory.
inline constexpr std::uint32_t kMaxTextureSide = 16384;

enum class TextureUsage : std::uint32_t { Color = 0, Normal = 1, Linear = 2 };
enum class TextureEncoding : std::uint32_t { Rgba8 = 0, Bc7 = 1, Bc5 = 2 };

struct TextureMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string data;
};

struct TexturePayload {
    TextureUsage usage = TextureUsage::Color;
    TextureEncoding encoding = TextureEncoding::Rgba8;
    bool srgb = false;
    std::vector<TextureMip> mips;  // mips[0] is the full image
};

// The number of mips in a full chain down to 1x1: one plus ceil(log2(larger side)).
[[nodiscard]] std::uint32_t full_mip_count(std::uint32_t width, std::uint32_t height) noexcept;

// The size of mip `level` along one axis, for a base size `size`.
[[nodiscard]] std::uint32_t mip_extent(std::uint32_t size, std::uint32_t level) noexcept;

// The bytes one mip of the given size occupies in the given encoding.
[[nodiscard]] std::size_t encoded_mip_bytes(TextureEncoding encoding, std::uint32_t width, std::uint32_t height) noexcept;

// The rules a payload must meet: sizes in range, a full mip chain with the right extents, byte counts that match the
// encoding, and usage, encoding, and color space that agree. The importer checks its output with this before writing it,
// and the reader checks every file with it.
[[nodiscard]] core::Result<core::Unit, std::string> validate_texture(const TexturePayload& texture);

// Writes a payload. It must already be valid; the writer does not repeat the checks, so the same texture always produces
// the same bytes.
[[nodiscard]] std::string write_texture_payload(const TexturePayload& texture);

// Reads a payload. Counts are checked against the bytes that remain before anything is allocated.
[[nodiscard]] core::Result<TexturePayload, std::string> read_texture_payload(std::string_view bytes);

// A 1x1 opaque magenta color texture. An asset handle returns it while the texture is not ready, so a missing texture is
// visible on screen rather than silently black.
[[nodiscard]] TexturePayload fallback_texture();

}  // namespace engine::assets
