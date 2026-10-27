#include "asset_converter/block_compression.hpp"
#include "asset_converter/converter.hpp"
#include "asset_converter/importer.hpp"
#include "engine/assets/texture_format.hpp"

#include <nlohmann/json.hpp>

// stb_image is vendored and included as a system header, so its warnings stay out of the first-party policy. Only the PNG
// and JPEG decoders are compiled, and there is no file IO: the importer decodes bytes the converter has already read.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::converter {
namespace {

using Json = nlohmann::json;
using core::Result;
using engine::assets::TextureEncoding;
using engine::assets::TextureMip;
using engine::assets::TexturePayload;
using engine::assets::TextureUsage;
using engine::assets::encoded_mip_bytes;
using engine::assets::full_mip_count;
using engine::assets::kMaxTextureSide;
using engine::assets::mip_extent;
using engine::assets::validate_texture;
using engine::assets::write_texture_payload;
using core::failure;
using core::success;

// Bump with the importer version when the mip rules or the encodings change.
constexpr int kTextureImporterVersion = 1;

constexpr std::string_view kPngSignature("\x89PNG\r\n\x1a\n", 8);
// A PNG ends with an IEND chunk: its length (zero), its type, and its CRC. A file cut short loses this chunk.
constexpr std::string_view kPngEnd("\0\0\0\0IEND\xAE\x42\x60\x82", 12);
// A JPEG ends with the end-of-image marker. A file cut short loses it.
constexpr std::string_view kJpegEnd("\xFF\xD9", 2);

// Decoded pixels: RGBA8, row-major, top row first. Mip levels reuse this layout.
struct Level {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct Settings {
    TextureUsage usage = TextureUsage::Color;
    bool rgba8_fallback = false;
};

// The sRGB transfer function. Color mips average in linear light, so the decode and encode go through these.
float srgb_to_linear(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float linear) {
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    return clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
}

std::uint8_t to_byte(float unit) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0f, 1.0f) * 255.0f));
}

// Normal texels hold n * 0.5 + 0.5 in each channel. The decode and encode invert that mapping.
std::array<float, 3> decode_normal(const std::uint8_t* texel) {
    return {static_cast<float>(texel[0]) / 255.0f * 2.0f - 1.0f, static_cast<float>(texel[1]) / 255.0f * 2.0f - 1.0f,
            static_cast<float>(texel[2]) / 255.0f * 2.0f - 1.0f};
}

void encode_normal(const std::array<float, 3>& normal, std::uint8_t* texel) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
        texel[channel] = to_byte(normal[channel] * 0.5f + 0.5f);
    }
}

// Renormalizes one normal texel. A zero vector has no direction, so it becomes the +Z normal.
std::array<float, 3> normalize_or_up(std::array<float, 3> normal) {
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (length < 1e-6f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {normal[0] / length, normal[1] / length, normal[2] / length};
}

bool starts_with(std::string_view bytes, std::string_view prefix) {
    return bytes.size() >= prefix.size() && bytes.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view bytes, std::string_view suffix) {
    return bytes.size() >= suffix.size() && bytes.substr(bytes.size() - suffix.size()) == suffix;
}

// Decodes a PNG or JPEG to RGBA8. Each format is recognized by its signature and must carry its end marker, so a file cut
// short is rejected. Bit depths above 8 are reduced to 8 bits by the decoder; the caller reports that as a note.
Result<Level, std::string> decode_image(std::string_view bytes, bool& sixteen_bit) {
    const bool png = starts_with(bytes, kPngSignature);
    const bool jpeg = !png && starts_with(bytes, "\xFF\xD8\xFF");
    if (!png && !jpeg) {
        return failure(std::string("unsupported image format: expected PNG or JPEG"));
    }
    if (png && !ends_with(bytes, kPngEnd)) {
        return failure(std::string("the PNG is truncated: its IEND chunk is missing"));
    }
    if (jpeg && !ends_with(bytes, kJpegEnd)) {
        return failure(std::string("the JPEG is truncated: its end-of-image marker is missing"));
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        return failure(std::string("the image file is larger than the decoder can read"));
    }

    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int length = static_cast<int>(bytes.size());
    sixteen_bit = stbi_is_16_bit_from_memory(data, length) != 0;

    int width = 0;
    int height = 0;
    int components = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, length, &width, &height, &components, 4);
    if (pixels == nullptr) {
        return failure(std::string("the image data is corrupt"));
    }
    Level level;
    level.width = static_cast<std::uint32_t>(width);
    level.height = static_cast<std::uint32_t>(height);
    if (width <= 0 || height <= 0 || level.width > kMaxTextureSide || level.height > kMaxTextureSide) {
        stbi_image_free(pixels);
        return failure("the image must be between 1 and " + std::to_string(kMaxTextureSide) + " pixels on each side");
    }
    const std::size_t count = static_cast<std::size_t>(level.width) * level.height * 4;
    level.pixels.assign(pixels, pixels + count);
    stbi_image_free(pixels);
    return success(std::move(level));
}

// The next mip is a 2x2 box filter over the previous one. Each usage averages in the space its data lives in: color in
// linear light, normals as vectors that are then renormalized, and linear data as plain numbers. Odd sides keep their
// last row or column by reading the edge texel twice.
Level downsample(const Level& source, TextureUsage usage) {
    Level next;
    next.width = std::max<std::uint32_t>(1, (source.width + 1) / 2);
    next.height = std::max<std::uint32_t>(1, (source.height + 1) / 2);
    next.pixels.resize(static_cast<std::size_t>(next.width) * next.height * 4);

    for (std::uint32_t y = 0; y < next.height; ++y) {
        for (std::uint32_t x = 0; x < next.width; ++x) {
            const std::uint32_t xs[2] = {std::min(2 * x, source.width - 1), std::min(2 * x + 1, source.width - 1)};
            const std::uint32_t ys[2] = {std::min(2 * y, source.height - 1), std::min(2 * y + 1, source.height - 1)};
            const std::uint8_t* texels[4] = {
                &source.pixels[4 * (static_cast<std::size_t>(ys[0]) * source.width + xs[0])],
                &source.pixels[4 * (static_cast<std::size_t>(ys[0]) * source.width + xs[1])],
                &source.pixels[4 * (static_cast<std::size_t>(ys[1]) * source.width + xs[0])],
                &source.pixels[4 * (static_cast<std::size_t>(ys[1]) * source.width + xs[1])],
            };
            std::uint8_t* out = &next.pixels[4 * (static_cast<std::size_t>(y) * next.width + x)];

            if (usage == TextureUsage::Normal) {
                std::array<float, 3> sum{0.0f, 0.0f, 0.0f};
                for (const std::uint8_t* texel : texels) {
                    const auto normal = decode_normal(texel);
                    for (std::size_t c = 0; c < 3; ++c) {
                        sum[c] += normal[c] * 0.25f;
                    }
                }
                encode_normal(normalize_or_up(sum), out);
                out[3] = static_cast<std::uint8_t>((texels[0][3] + texels[1][3] + texels[2][3] + texels[3][3] + 2) / 4);
                continue;
            }
            for (std::size_t c = 0; c < 4; ++c) {
                const bool color_channel = usage == TextureUsage::Color && c < 3;
                float sum = 0.0f;
                for (const std::uint8_t* texel : texels) {
                    sum += color_channel ? srgb_to_linear(static_cast<float>(texel[c]) / 255.0f)
                                         : static_cast<float>(texel[c]) / 255.0f;
                }
                const float mean = sum * 0.25f;
                out[c] = to_byte(color_channel ? linear_to_srgb(mean) : mean);
            }
        }
    }
    return next;
}

// Renormalizes every texel of a normal map before the chain is built, so mip 0 is already unit length.
void renormalize(Level& level) {
    for (std::size_t i = 0; i < level.pixels.size(); i += 4) {
        encode_normal(normalize_or_up(decode_normal(&level.pixels[i])), &level.pixels[i]);
    }
}

// Encodes one level. Partial blocks at the edge repeat the last row and column, as the payload format describes.
std::string encode_level(const Level& level, TextureEncoding encoding, bool perceptual) {
    if (encoding == TextureEncoding::Rgba8) {
        return std::string(level.pixels.begin(), level.pixels.end());
    }
    const std::uint32_t blocks_x = (level.width + 3) / 4;
    const std::uint32_t blocks_y = (level.height + 3) / 4;
    std::string out(static_cast<std::size_t>(blocks_x) * blocks_y * kBlockBytes, '\0');
    std::uint8_t texels[kBlockTexels * 4];
    auto* block = reinterpret_cast<std::uint8_t*>(out.data());

    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            for (std::uint32_t j = 0; j < 4; ++j) {
                for (std::uint32_t i = 0; i < 4; ++i) {
                    const std::uint32_t x = std::min(bx * 4 + i, level.width - 1);
                    const std::uint32_t y = std::min(by * 4 + j, level.height - 1);
                    const std::uint8_t* src = &level.pixels[4 * (static_cast<std::size_t>(y) * level.width + x)];
                    std::copy(src, src + 4, &texels[4 * (j * 4 + i)]);
                }
            }
            std::uint8_t* dst = block + 16 * (static_cast<std::size_t>(by) * blocks_x + bx);
            if (encoding == TextureEncoding::Bc5) {
                encode_bc5_block(texels, dst);
            } else {
                encode_bc7_block(texels, dst, perceptual);
            }
        }
    }
    return out;
}

Result<Settings, std::string> parse_settings(std::string_view text) {
    Json document = Json::parse(std::string(text), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return failure(std::string("settings must be a JSON object"));
    }
    Settings settings;
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (it.key() == "usage") {
            if (!it->is_string()) {
                return failure(std::string("setting 'usage' must be one of color, normal, linear"));
            }
            const std::string usage = it->get<std::string>();
            if (usage == "color") {
                settings.usage = TextureUsage::Color;
            } else if (usage == "normal") {
                settings.usage = TextureUsage::Normal;
            } else if (usage == "linear") {
                settings.usage = TextureUsage::Linear;
            } else {
                return failure(std::string("setting 'usage' must be one of color, normal, linear"));
            }
        } else if (it.key() == "format") {
            if (!it->is_string()) {
                return failure(std::string("setting 'format' must be one of auto, rgba8"));
            }
            const std::string format = it->get<std::string>();
            if (format == "auto") {
                settings.rgba8_fallback = false;
            } else if (format == "rgba8") {
                settings.rgba8_fallback = true;
            } else {
                return failure(std::string("setting 'format' must be one of auto, rgba8"));
            }
        } else {
            return failure("unknown setting '" + it.key() + "'");
        }
    }
    return success(settings);
}

class TextureImporter final : public Importer {
public:
    [[nodiscard]] std::string_view type() const noexcept override {
        return "texture";
    }
    [[nodiscard]] int version() const noexcept override {
        return kTextureImporterVersion;
    }

    // Defaults are filled in, so "{}" and the spelled-out form share cache entries.
    [[nodiscard]] Result<std::string, std::string> canonical_settings(std::string_view settings_json) const override {
        auto parsed = parse_settings(settings_json);
        if (!parsed.ok()) {
            return failure(parsed.error());
        }
        const Settings& settings = parsed.value();
        const char* usage = settings.usage == TextureUsage::Color    ? "color"
                            : settings.usage == TextureUsage::Normal ? "normal"
                                                                     : "linear";
        Json canonical = Json::object();
        canonical["format"] = settings.rgba8_fallback ? "rgba8" : "auto";
        canonical["usage"] = usage;
        return success(canonical.dump());
    }

    [[nodiscard]] Result<ImportOutput, std::string> import(std::string_view source,
                                                           std::string_view settings_json) const override {
        auto parsed = parse_settings(settings_json);
        if (!parsed.ok()) {
            return failure(parsed.error());
        }
        const Settings settings = parsed.value();

        bool sixteen_bit = false;
        auto decoded = decode_image(source, sixteen_bit);
        if (!decoded.ok()) {
            return failure(decoded.error());
        }
        Level base = std::move(decoded).value();
        if (settings.usage == TextureUsage::Normal) {
            renormalize(base);
        }

        // Every level of the chain is kept until it is encoded, so the payload is built from the full chain at once.
        std::vector<Level> chain;
        chain.push_back(std::move(base));
        const std::uint32_t mip_count = full_mip_count(chain.front().width, chain.front().height);
        while (chain.size() < mip_count) {
            chain.push_back(downsample(chain.back(), settings.usage));
        }

        TexturePayload texture;
        texture.usage = settings.usage;
        texture.srgb = settings.usage == TextureUsage::Color;
        if (settings.rgba8_fallback) {
            texture.encoding = TextureEncoding::Rgba8;
        } else if (settings.usage == TextureUsage::Normal) {
            texture.encoding = TextureEncoding::Bc5;
        } else {
            texture.encoding = TextureEncoding::Bc7;
        }
        const bool perceptual = settings.usage == TextureUsage::Color;
        for (const Level& level : chain) {
            texture.mips.push_back({level.width, level.height, encode_level(level, texture.encoding, perceptual)});
        }

        auto valid = validate_texture(texture);
        if (!valid.ok()) {
            return failure("internal texture error: " + valid.error());
        }

        ImportOutput output{write_texture_payload(texture), {}};
        if (sixteen_bit) {
            output.unsupported.push_back("16-bit samples were reduced to 8 bits");
        }
        return success(std::move(output));
    }
};

}  // namespace

Result<TexturePreview, std::string> make_texture_preview(std::string_view bytes, std::uint32_t max_side) {
    if (max_side == 0) {
        return failure(std::string("the preview size must be at least one pixel"));
    }
    bool sixteen_bit = false;
    auto decoded = decode_image(bytes, sixteen_bit);
    if (!decoded.ok()) {
        return failure(decoded.error());
    }
    const Level& level = decoded.value();
    // Nearest-sample reduction: a preview only has to show the image, and it keeps the aspect ratio.
    const std::uint32_t longest = std::max(level.width, level.height);
    const std::uint32_t scale = std::max<std::uint32_t>(1, (longest + max_side - 1) / max_side);
    TexturePreview preview;
    preview.width = std::max<std::uint32_t>(1, level.width / scale);
    preview.height = std::max<std::uint32_t>(1, level.height / scale);
    preview.pixels.resize(static_cast<std::size_t>(preview.width) * preview.height * 4);
    for (std::uint32_t y = 0; y < preview.height; ++y) {
        for (std::uint32_t x = 0; x < preview.width; ++x) {
            const std::size_t source = (static_cast<std::size_t>(y * scale) * level.width + x * scale) * 4;
            const std::size_t target = (static_cast<std::size_t>(y) * preview.width + x) * 4;
            std::copy_n(level.pixels.begin() + static_cast<std::ptrdiff_t>(source), 4,
                        preview.pixels.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    return success(std::move(preview));
}

std::unique_ptr<Importer> make_texture_importer() {
    return std::make_unique<TextureImporter>();
}

}  // namespace engine::converter
