#include "asset_converter/block_compression.hpp"

// bc7decomp is vendored and is included as a system header, so its warnings stay out of the first-party policy. It lives
// in its own file because bc7enc.h defines a global color_rgba that would clash with the bc7decomp class in one unit.
#include <bc7decomp.h>

#include <cstddef>

namespace engine::converter {

bool decode_bc7_block(const std::uint8_t* block, std::uint8_t* texels) {
    bc7decomp::color_rgba pixels[kBlockTexels];
    if (!bc7decomp::unpack_bc7(block, pixels)) {
        return false;
    }
    for (int i = 0; i < kBlockTexels; ++i) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            texels[4 * i + channel] = pixels[i].m_comps[channel];
        }
    }
    return true;
}

}  // namespace engine::converter
