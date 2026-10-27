#pragma once

// SHA-256 digest of a byte sequence (FIPS 180-4). The registry compares digests to detect a changed source. It is not a
// security boundary, and it does not need to be one.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::assets {

class ContentHash {
public:
    using Bytes = std::array<std::uint8_t, 32>;

    ContentHash() noexcept = default;
    explicit ContentHash(const Bytes& bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] static ContentHash of(std::string_view bytes);

    // Lowercase hexadecimal, 64 characters.
    [[nodiscard]] std::string to_hex() const;

    [[nodiscard]] const Bytes& bytes() const noexcept {
        return bytes_;
    }

    friend bool operator==(const ContentHash& lhs, const ContentHash& rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }
    friend bool operator!=(const ContentHash& lhs, const ContentHash& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    Bytes bytes_{};
};

}  // namespace engine::assets
