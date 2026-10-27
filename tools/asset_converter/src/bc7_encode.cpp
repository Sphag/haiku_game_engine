#include "asset_converter/block_compression.hpp"

// bc7enc_rdo is vendored and is included as a system header, so its warnings stay out of the first-party policy. Its
// header defines a global color_rgba, so the decoder lives in its own file (bc7_decode.cpp).
#include <bc7enc.h>

namespace engine::converter {

void encode_bc7_block(const std::uint8_t* texels, std::uint8_t* block, bool perceptual_weights) {
    // The encoder's tables must be built once before the first block. A function-local static initializes thread-safely,
    // and conversion jobs encode blocks in parallel.
    static const bool initialized = [] {
        bc7enc_compress_block_init();
        return true;
    }();
    (void)initialized;

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (!perceptual_weights) {
        bc7enc_compress_block_params_init_linear_weights(&params);
    }
    // The encoder's return value is not a success flag: it reports whether the block has any alpha below 255. Every call
    // writes a block.
    (void)bc7enc_compress_block(block, texels, &params);
}

}  // namespace engine::converter
