#pragma once

// A read-only view of bytes. The local toolchain's standard library has no <span> (GCC 9 libstdc++), so the audio module uses this
// small view in its place. It does not own the bytes.

#include <cstddef>
#include <cstdint>

namespace engine::audio {

class ByteView {
public:
    constexpr ByteView() noexcept = default;
    constexpr ByteView(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] constexpr const std::uint8_t* data() const noexcept {
        return data_;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] constexpr const std::uint8_t* begin() const noexcept {
        return data_;
    }
    [[nodiscard]] constexpr const std::uint8_t* end() const noexcept {
        return data_ + size_;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace engine::audio
