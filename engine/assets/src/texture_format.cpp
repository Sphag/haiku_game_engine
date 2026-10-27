#include "engine/assets/texture_format.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace engine::assets {
namespace {

using core::Result;
using core::Unit;
using core::failure;
using core::success;

constexpr std::uint32_t kUsageCount = 3;
constexpr std::uint32_t kEncodingCount = 3;
// A 16384-pixel side has 15 mips. Anything above this bound cannot be a valid chain.
constexpr std::uint32_t kMaxMipCount = 32;
constexpr std::size_t kMipHeaderBytes = 4 + 4 + 4;

struct ReadError {
    std::string message;
};

class Writer {
public:
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out_.push_back(static_cast<char>((value >> shift) & 0xFFu));
        }
    }
    void bytes(std::string_view value) {
        out_.append(value.data(), value.size());
    }
    [[nodiscard]] std::string take() {
        return std::move(out_);
    }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - pos_;
    }

    [[nodiscard]] bool at_end() const noexcept {
        return pos_ == bytes_.size();
    }

    void need(std::size_t count) const {
        if (count > remaining()) {
            throw ReadError{"the payload ends early"};
        }
    }

    std::uint32_t u32() {
        need(4);
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes_[pos_++])) << shift;
        }
        return value;
    }

    std::string_view take(std::size_t count) {
        need(count);
        std::string_view view = bytes_.substr(pos_, count);
        pos_ += count;
        return view;
    }

private:
    std::string_view bytes_;
    std::size_t pos_ = 0;
};

// Checks one mip's extent and byte count against the base size and encoding.
Result<Unit, std::string> check_mip(const TextureMip& mip, std::uint32_t level, std::uint32_t base_width,
                                    std::uint32_t base_height, TextureEncoding encoding) {
    const std::uint32_t width = mip_extent(base_width, level);
    const std::uint32_t height = mip_extent(base_height, level);
    if (mip.width != width || mip.height != height) {
        return failure("mip " + std::to_string(level) + " is " + std::to_string(mip.width) + "x" +
                       std::to_string(mip.height) + ", expected " + std::to_string(width) + "x" +
                       std::to_string(height));
    }
    const std::size_t expected = encoded_mip_bytes(encoding, width, height);
    if (mip.data.size() != expected) {
        return failure("mip " + std::to_string(level) + " holds " + std::to_string(mip.data.size()) + " bytes, expected " +
                       std::to_string(expected));
    }
    return success();
}

}  // namespace

std::uint32_t full_mip_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t larger = width > height ? width : height;
    std::uint32_t count = 1;
    while (larger > 1) {
        larger = (larger + 1) / 2;
        ++count;
    }
    return count;
}

std::uint32_t mip_extent(std::uint32_t size, std::uint32_t level) noexcept {
    const std::uint32_t halved = level >= 32 ? 0 : (size + ((1u << level) - 1u)) >> level;
    return halved == 0 ? 1 : halved;
}

std::size_t encoded_mip_bytes(TextureEncoding encoding, std::uint32_t width, std::uint32_t height) noexcept {
    const std::size_t w = width;
    const std::size_t h = height;
    switch (encoding) {
        case TextureEncoding::Rgba8:
            return w * h * 4;
        case TextureEncoding::Bc7:
        case TextureEncoding::Bc5:
            // Each 4x4 block is 16 bytes in both block formats.
            return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    }
    return 0;
}

core::Result<core::Unit, std::string> validate_texture(const TexturePayload& texture) {
    if (texture.mips.empty()) {
        return failure(std::string("the texture has no mips"));
    }
    const TextureMip& base = texture.mips.front();
    if (base.width == 0 || base.height == 0 || base.width > kMaxTextureSide || base.height > kMaxTextureSide) {
        return failure("the texture size must be between 1 and " + std::to_string(kMaxTextureSide) + " on each side");
    }
    if (static_cast<std::uint32_t>(texture.usage) >= kUsageCount) {
        return failure(std::string("the texture usage is unknown"));
    }
    if (static_cast<std::uint32_t>(texture.encoding) >= kEncodingCount) {
        return failure(std::string("the texture encoding is unknown"));
    }
    if (texture.encoding == TextureEncoding::Bc5 && texture.usage != TextureUsage::Normal) {
        return failure(std::string("bc5 stores normals only and requires the normal usage"));
    }
    if (texture.encoding == TextureEncoding::Bc7 && texture.usage == TextureUsage::Normal) {
        return failure(std::string("normal data is stored as bc5 or rgba8, not bc7"));
    }
    if (texture.srgb != (texture.usage == TextureUsage::Color)) {
        return failure(std::string("only color data is sRGB; normal and linear data are linear"));
    }

    const std::uint32_t expected_count = full_mip_count(base.width, base.height);
    if (texture.mips.size() != expected_count) {
        return failure("the texture has " + std::to_string(texture.mips.size()) + " mips, expected " +
                       std::to_string(expected_count));
    }
    for (std::uint32_t level = 0; level < expected_count; ++level) {
        auto checked = check_mip(texture.mips[level], level, base.width, base.height, texture.encoding);
        if (!checked.ok()) {
            return failure(checked.error());
        }
    }
    return success();
}

std::string write_texture_payload(const TexturePayload& texture) {
    Writer out;
    out.bytes(kTexturePayloadMagic);
    out.u32(kTexturePayloadVersion);
    out.u32(static_cast<std::uint32_t>(texture.usage));
    out.u32(static_cast<std::uint32_t>(texture.encoding));
    out.u32(texture.srgb ? 1u : 0u);
    out.u32(static_cast<std::uint32_t>(texture.mips.size()));
    for (const TextureMip& mip : texture.mips) {
        out.u32(mip.width);
        out.u32(mip.height);
        out.u32(static_cast<std::uint32_t>(mip.data.size()));
        out.bytes(mip.data);
    }
    return out.take();
}

core::Result<TexturePayload, std::string> read_texture_payload(std::string_view bytes) {
    try {
        Reader in(bytes);
        if (in.take(kTexturePayloadMagic.size()) != kTexturePayloadMagic) {
            throw ReadError{"the file is not a texture payload"};
        }
        if (in.u32() != kTexturePayloadVersion) {
            throw ReadError{"the texture payload version is not supported"};
        }

        TexturePayload texture;
        const std::uint32_t usage = in.u32();
        const std::uint32_t encoding = in.u32();
        const std::uint32_t srgb = in.u32();
        if (usage >= kUsageCount || encoding >= kEncodingCount || srgb > 1) {
            throw ReadError{"the texture header holds an unknown usage, encoding, or color space"};
        }
        texture.usage = static_cast<TextureUsage>(usage);
        texture.encoding = static_cast<TextureEncoding>(encoding);
        texture.srgb = srgb == 1;

        const std::uint32_t mip_count = in.u32();
        if (mip_count == 0 || mip_count > kMaxMipCount) {
            throw ReadError{"the mip count is out of range"};
        }
        // Each mip has a header of at least kMipHeaderBytes, so the count is checked against the bytes before the loop.
        if (static_cast<std::size_t>(mip_count) > in.remaining() / kMipHeaderBytes) {
            throw ReadError{"the mip count is larger than the payload"};
        }
        texture.mips.reserve(mip_count);
        for (std::uint32_t level = 0; level < mip_count; ++level) {
            TextureMip mip;
            mip.width = in.u32();
            mip.height = in.u32();
            const std::uint32_t size = in.u32();
            mip.data = std::string(in.take(size));
            texture.mips.push_back(std::move(mip));
        }
        if (!in.at_end()) {
            throw ReadError{"the payload has bytes after the last mip"};
        }

        auto valid = validate_texture(texture);
        if (!valid.ok()) {
            throw ReadError{valid.error()};
        }
        return success(std::move(texture));
    } catch (const ReadError& error) {
        return failure(error.message);
    }
}

TexturePayload fallback_texture() {
    TexturePayload texture;
    texture.usage = TextureUsage::Color;
    texture.encoding = TextureEncoding::Rgba8;
    texture.srgb = true;
    texture.mips.push_back(TextureMip{1, 1, std::string{'\xFF', '\x00', '\xFF', '\xFF'}});
    return texture;
}

}  // namespace engine::assets
