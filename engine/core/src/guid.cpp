#include "engine/core/guid.hpp"

#include <random>

namespace engine::core {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

// Hyphens separate the 8-4-4-4-12 groups of the canonical text form.
bool is_hyphen_position(std::size_t index) {
    return index == 8 || index == 13 || index == 18 || index == 23;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::mt19937_64& random_engine() {
    thread_local std::mt19937_64 engine = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    return engine;
}

}  // namespace

Guid Guid::generate() {
    auto& engine = random_engine();
    Bytes bytes{};
    for (std::size_t word = 0; word < 2; ++word) {
        const std::uint64_t value = engine();
        for (std::size_t i = 0; i < 8; ++i) {
            bytes[word * 8 + i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
        }
    }
    // Version 4 in the high nibble of byte 6; RFC 4122 variant in the top two bits of byte 8.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    return Guid(bytes);
}

Result<Guid, GuidError> Guid::parse(std::string_view text) {
    constexpr std::size_t kTextLength = 36;
    if (text.size() != kTextLength) {
        return failure(GuidError::InvalidFormat);
    }

    Bytes bytes{};
    std::size_t out = 0;
    std::size_t i = 0;
    while (i < kTextLength) {
        if (is_hyphen_position(i)) {
            if (text[i] != '-') {
                return failure(GuidError::InvalidFormat);
            }
            ++i;
            continue;
        }
        const int high = hex_value(text[i]);
        const int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) {
            return failure(GuidError::InvalidFormat);
        }
        bytes[out++] = static_cast<std::uint8_t>((high << 4) | low);
        i += 2;
    }
    return success(Guid(bytes));
}

std::string Guid::to_string() const {
    std::string text;
    text.reserve(36);
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text += '-';
        }
        text += kHexDigits[bytes_[i] >> 4];
        text += kHexDigits[bytes_[i] & 0x0F];
    }
    return text;
}

}  // namespace engine::core
