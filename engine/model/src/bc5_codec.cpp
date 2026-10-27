#include "asset_converter/block_compression.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>

namespace engine::converter {
namespace {

constexpr std::size_t kBc4BlockBytes = 8;

// The eight BC4 palette values for two endpoints. red0 > red1 selects the eight-value mode, where six values interpolate
// between the endpoints. Otherwise the six-value mode interpolates four values and adds the constants 0 and 255.
std::array<int, 8> bc4_palette(int red0, int red1) {
    std::array<int, 8> palette{};
    palette[0] = red0;
    palette[1] = red1;
    if (red0 > red1) {
        for (int k = 2; k < 8; ++k) {
            palette[static_cast<std::size_t>(k)] = ((8 - k) * red0 + (k - 1) * red1 + 3) / 7;
        }
    } else {
        for (int k = 2; k < 6; ++k) {
            palette[static_cast<std::size_t>(k)] = ((6 - k) * red0 + (k - 1) * red1 + 2) / 5;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    return palette;
}

// Encodes 16 values of one channel as a BC4 block. The endpoints are the channel's minimum and maximum, and each value
// takes the nearest palette entry. A flat block has equal endpoints, so every index is 0 and the value is exact.
void encode_bc4(const std::uint8_t* values, std::uint8_t* block) {
    int low = 255;
    int high = 0;
    for (int i = 0; i < kBlockTexels; ++i) {
        const int value = values[i];
        low = value < low ? value : low;
        high = value > high ? value : high;
    }
    const auto palette = bc4_palette(high, low);

    std::uint64_t indices = 0;
    for (int i = 0; i < kBlockTexels; ++i) {
        int best = 0;
        int best_error = 1 << 30;
        for (int k = 0; k < 8; ++k) {
            const int error = std::abs(values[i] - palette[static_cast<std::size_t>(k)]);
            if (error < best_error) {
                best_error = error;
                best = k;
            }
        }
        indices |= static_cast<std::uint64_t>(best) << (3 * i);
    }

    block[0] = static_cast<std::uint8_t>(high);
    block[1] = static_cast<std::uint8_t>(low);
    // The 48 index bits are little-endian, texel 0 in the lowest bits.
    for (std::size_t byte = 0; byte < 6; ++byte) {
        block[2 + byte] = static_cast<std::uint8_t>((indices >> (8 * byte)) & 0xFFu);
    }
}

void decode_bc4(const std::uint8_t* block, std::uint8_t* values) {
    const auto palette = bc4_palette(block[0], block[1]);
    std::uint64_t indices = 0;
    for (std::size_t byte = 0; byte < 6; ++byte) {
        indices |= static_cast<std::uint64_t>(block[2 + byte]) << (8 * byte);
    }
    for (int i = 0; i < kBlockTexels; ++i) {
        const auto index = static_cast<std::size_t>((indices >> (3 * i)) & 7u);
        values[i] = static_cast<std::uint8_t>(palette[index]);
    }
}

}  // namespace

void encode_bc5_block(const std::uint8_t* texels, std::uint8_t* block) noexcept {
    std::uint8_t red[kBlockTexels];
    std::uint8_t green[kBlockTexels];
    for (int i = 0; i < kBlockTexels; ++i) {
        red[i] = texels[4 * i + 0];
        green[i] = texels[4 * i + 1];
    }
    encode_bc4(red, block);
    encode_bc4(green, block + kBc4BlockBytes);
}

void decode_bc5_block(const std::uint8_t* block, std::uint8_t* texels) noexcept {
    std::uint8_t red[kBlockTexels];
    std::uint8_t green[kBlockTexels];
    decode_bc4(block, red);
    decode_bc4(block + kBc4BlockBytes, green);
    for (int i = 0; i < kBlockTexels; ++i) {
        texels[4 * i + 0] = red[i];
        texels[4 * i + 1] = green[i];
        texels[4 * i + 2] = 0;
        texels[4 * i + 3] = 255;
    }
}

}  // namespace engine::converter
