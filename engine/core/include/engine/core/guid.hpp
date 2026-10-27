#pragma once

#include "engine/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace engine::core {

enum class GuidError {
    InvalidFormat,
};

// 128-bit identifier. Persistent objects (scene nodes, assets) store it in canonical text form.
class Guid {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    Guid() noexcept = default;
    explicit Guid(const Bytes& bytes) noexcept : bytes_(bytes) {}

    // RFC 4122 version 4 (random). Uniqueness relies on the platform random source, not on secrecy.
    [[nodiscard]] static Guid generate();

    // Accepts the canonical 8-4-4-4-12 form in either letter case.
    [[nodiscard]] static Result<Guid, GuidError> parse(std::string_view text);

    // Lowercase canonical form, for example "3f2504e0-4f89-41d3-9a0c-0305e82c3301".
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool is_nil() const noexcept {
        for (const auto byte : bytes_) {
            if (byte != 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const Bytes& bytes() const noexcept {
        return bytes_;
    }

    friend bool operator==(const Guid& lhs, const Guid& rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }
    friend bool operator!=(const Guid& lhs, const Guid& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const Guid& lhs, const Guid& rhs) noexcept {
        return lhs.bytes_ < rhs.bytes_;
    }

private:
    Bytes bytes_{};
};

}  // namespace engine::core

template <>
struct std::hash<engine::core::Guid> {
    std::size_t operator()(const engine::core::Guid& guid) const noexcept {
        std::size_t combined = 0;
        for (const auto byte : guid.bytes()) {
            combined = combined * 131 + byte;
        }
        return combined;
    }
};
