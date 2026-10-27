#pragma once

// 4x4 block codecs for cooked textures (US-021). The PRD assigns the BC5 encoder to the converter, so BC4/BC5 is encoded
// and decoded here; BC7 uses the vendored bc7enc_rdo encoder and bc7decomp decoder. Decoders exist so the tests can
// check what the encoders wrote, and so a later stage can verify cooked output.
//
// A block is 16 RGBA8 texels in row-major order (64 bytes) and encodes to 16 bytes. Decoded texels are RGBA8 in the same
// order. Encoders read only the channels their format stores: BC5 reads R and G, and BC7 reads all four.

#include <cstdint>

namespace engine::converter {

inline constexpr int kBlockTexels = 16;
inline constexpr int kBlockBytes = 16;

// BC5: the R channel in the first 8 bytes and the G channel in the next 8, each as a BC4 block. The decoded texel has
// R and G from the block, B = 0, and A = 255.
void encode_bc5_block(const std::uint8_t* texels, std::uint8_t* block) noexcept;
void decode_bc5_block(const std::uint8_t* block, std::uint8_t* texels) noexcept;

// BC7: `perceptual_weights` selects the error weighting used for color; false weights the channels equally, which suits
// linear data. The encoder always writes a block. Decoding fails only for a block that is not valid BC7.
void encode_bc7_block(const std::uint8_t* texels, std::uint8_t* block, bool perceptual_weights);
[[nodiscard]] bool decode_bc7_block(const std::uint8_t* block, std::uint8_t* texels);

}  // namespace engine::converter
