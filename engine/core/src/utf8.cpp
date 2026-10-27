#include "engine/core/utf8.hpp"

namespace engine::core {

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t size = text.size();
    while (i < size) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }

        // The second byte has tighter bounds for E0, ED, F0, and F4 so overlong forms and surrogates are rejected.
        std::size_t length = 0;
        unsigned char second_low = 0x80;
        unsigned char second_high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0) {
                second_low = 0xA0;
            } else if (lead == 0xED) {
                second_high = 0x9F;
            }
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0) {
                second_low = 0x90;
            } else if (lead == 0xF4) {
                second_high = 0x8F;
            }
        } else {
            return false;
        }

        if (size - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            const unsigned char low = (k == 1) ? second_low : 0x80;
            const unsigned char high = (k == 1) ? second_high : 0xBF;
            if (next < low || next > high) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

}  // namespace engine::core
