// Texture conversion (US-021): the cooked texture payload, the BC4/BC5 and BC7 codecs, and the texture importer. The
// fixtures in tests/fixtures/textures are small enough that the expected values below are worked out by hand, and
// payloads are read back with read_texture_payload, which applies the same checks the converter relies on.

#include "asset_converter/block_compression.hpp"
#include "asset_converter/converter.hpp"
#include "asset_converter/importer.hpp"
#include "engine/assets/texture_format.hpp"

#include "engine/core/guid.hpp"
#include "engine/core/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using engine::converter::decode_bc5_block;
using engine::converter::decode_bc7_block;
using engine::converter::encode_bc5_block;
using engine::converter::encode_bc7_block;
using engine::assets::full_mip_count;
using engine::converter::ImportOutput;
using engine::converter::make_texture_importer;
using engine::converter::Command;
using engine::converter::AssetStatus;
using engine::converter::Options;
using engine::assets::read_texture_payload;
using engine::core::Guid;
using engine::core::Path;
using engine::assets::mip_extent;
using engine::assets::encoded_mip_bytes;
using engine::assets::TextureEncoding;
using engine::assets::TextureMip;
using engine::assets::TexturePayload;
using engine::assets::TextureUsage;
using engine::assets::validate_texture;
using engine::assets::write_texture_payload;
namespace fs = std::filesystem;

namespace {

std::string read_bytes(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream bytes;
    bytes << stream.rdbuf();
    return bytes.str();
}

// The cooked container: magic "OWCK", a 4-byte container version, an 8-byte payload size, and the payload.
constexpr std::size_t kContainerHeaderBytes = 16;

std::string utf8(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string texture_fixture(const char* name) {
    return read_bytes(fs::path(std::string(ENGINE_TEST_FIXTURE_DIR)) / "textures" / name);
}

// Imports a source with the texture importer, the way the converter does: canonical settings first, then the import.
ImportOutput import_texture(const std::string& source, const std::string& settings_json = "{}") {
    const auto importer = make_texture_importer();
    auto canonical = importer->canonical_settings(settings_json);
    INFO("settings rejected: " << (canonical.ok() ? "" : canonical.error()));
    REQUIRE(canonical.ok());
    auto imported = importer->import(source, canonical.value());
    INFO("import failed: " << (imported.ok() ? "" : imported.error()));
    REQUIRE(imported.ok());
    return std::move(imported).value();
}

TexturePayload read_payload(const std::string& bytes) {
    auto texture = read_texture_payload(bytes);
    INFO("payload rejected: " << (texture.ok() ? "" : texture.error()));
    REQUIRE(texture.ok());
    return std::move(texture).value();
}

std::uint8_t byte_at(const std::string& data, std::size_t index) {
    return static_cast<std::uint8_t>(data[index]);
}

// The unit-length check for a normal texel: the decoded vector should have length 1 to within the 8-bit quantization.
float normal_length(const std::string& data, std::size_t texel) {
    float sum = 0.0f;
    for (std::size_t c = 0; c < 3; ++c) {
        const float v = static_cast<float>(byte_at(data, 4 * texel + c)) / 255.0f * 2.0f - 1.0f;
        sum += v * v;
    }
    return std::sqrt(sum);
}

}  // namespace

TEST_CASE("Texture mips: the chain length and every extent follow ceil halving, including odd sizes", "[unit]") {
    CHECK(full_mip_count(1, 1) == 1);
    CHECK(full_mip_count(5, 3) == 4);
    CHECK(full_mip_count(9, 7) == 5);
    CHECK(full_mip_count(1024, 1) == 11);
    CHECK(full_mip_count(16384, 16384) == 15);

    // 5 -> 3 -> 2 -> 1, and a level past the end stays at 1.
    CHECK(mip_extent(5, 0) == 5);
    CHECK(mip_extent(5, 1) == 3);
    CHECK(mip_extent(5, 2) == 2);
    CHECK(mip_extent(5, 3) == 1);
    CHECK(mip_extent(5, 9) == 1);
    CHECK(mip_extent(1, 0) == 1);
}

TEST_CASE("Texture mips: the byte size of a mip depends on its encoding and pads partial blocks", "[unit]") {
    CHECK(encoded_mip_bytes(TextureEncoding::Rgba8, 5, 3) == 60);
    // 5x3 covers two blocks across and one down, padded to 4x4 blocks.
    CHECK(encoded_mip_bytes(TextureEncoding::Bc7, 5, 3) == 32);
    CHECK(encoded_mip_bytes(TextureEncoding::Bc5, 4, 4) == 16);
    CHECK(encoded_mip_bytes(TextureEncoding::Bc7, 1, 1) == 16);
    CHECK(encoded_mip_bytes(TextureEncoding::Bc7, 9, 7) == 96);
}

TEST_CASE("Texture payload: write and read keep every mip, and the validation rules reject bad textures", "[unit]") {
    // A 5x3 normal map in BC5: four mips, each with the size its extent and encoding give.
    TexturePayload texture;
    texture.usage = TextureUsage::Normal;
    texture.encoding = TextureEncoding::Bc5;
    texture.srgb = false;
    const std::uint32_t sizes[4][2] = {{5, 3}, {3, 2}, {2, 1}, {1, 1}};
    for (const auto& size : sizes) {
        TextureMip mip;
        mip.width = size[0];
        mip.height = size[1];
        mip.data.assign(encoded_mip_bytes(TextureEncoding::Bc5, size[0], size[1]), '\x5a');
        texture.mips.push_back(std::move(mip));
    }
    REQUIRE(validate_texture(texture).ok());

    const std::string bytes = write_texture_payload(texture);
    const TexturePayload back = read_payload(bytes);
    CHECK(back.usage == TextureUsage::Normal);
    CHECK(back.encoding == TextureEncoding::Bc5);
    CHECK_FALSE(back.srgb);
    REQUIRE(back.mips.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(back.mips[i].width == texture.mips[i].width);
        CHECK(back.mips[i].height == texture.mips[i].height);
        CHECK(back.mips[i].data == texture.mips[i].data);
    }

    SECTION("a mip with the wrong extent is rejected") {
        TexturePayload bad = texture;
        bad.mips[1].width = 2;
        CHECK_FALSE(validate_texture(bad).ok());
    }
    SECTION("a mip with the wrong byte count is rejected") {
        TexturePayload bad = texture;
        bad.mips[0].data.pop_back();
        CHECK_FALSE(validate_texture(bad).ok());
    }
    SECTION("a chain that stops early is rejected") {
        TexturePayload bad = texture;
        bad.mips.pop_back();
        CHECK_FALSE(validate_texture(bad).ok());
    }
    SECTION("BC5 is only for normals, and BC7 is not for normals") {
        TexturePayload color = texture;
        color.usage = TextureUsage::Color;
        color.srgb = true;
        CHECK_FALSE(validate_texture(color).ok());

        TexturePayload normal_bc7 = texture;
        normal_bc7.encoding = TextureEncoding::Bc7;
        CHECK_FALSE(validate_texture(normal_bc7).ok());
    }
    SECTION("only color data is sRGB") {
        TexturePayload srgb_normal = texture;
        srgb_normal.srgb = true;
        CHECK_FALSE(validate_texture(srgb_normal).ok());
    }
}

TEST_CASE("Texture payload: the reader rejects corrupt, truncated, and oversized files before it allocates", "[unit]") {
    TexturePayload texture;
    texture.usage = TextureUsage::Linear;
    texture.encoding = TextureEncoding::Rgba8;
    texture.mips.push_back({2, 2, std::string(16, '\x10')});
    texture.mips.push_back({1, 1, std::string(4, '\x20')});
    const std::string bytes = write_texture_payload(texture);
    REQUIRE(read_texture_payload(bytes).ok());

    CHECK_FALSE(read_texture_payload("").ok());
    CHECK_FALSE(read_texture_payload(bytes.substr(0, bytes.size() - 1)).ok());
    CHECK_FALSE(read_texture_payload(bytes + "x").ok());

    std::string wrong_magic = bytes;
    wrong_magic[0] = 'Q';
    CHECK_FALSE(read_texture_payload(wrong_magic).ok());

    // The mip count sits after the four header words: magic (4), version (4), usage, encoding, srgb (12).
    std::string huge_count = bytes;
    huge_count.replace(4 + 4 + 12, 4, std::string("\xFF\xFF\xFF\xFF", 4));
    CHECK_FALSE(read_texture_payload(huge_count).ok());

    // A mip size field larger than the rest of the file is rejected before its bytes are copied.
    std::string huge_size = bytes;
    huge_size.replace(4 + 4 + 12 + 4 + 8, 4, std::string("\xFF\xFF\x00\x00", 4));
    CHECK_FALSE(read_texture_payload(huge_size).ok());
}

TEST_CASE("BC5 codec: a flat block is exact, a gradient stays within one palette step, and the indices decode as worked by hand", "[unit]") {
    // Eight-value mode with red0 = 255 and red1 = 0, every texel at index 7: (1 * 255 + 6 * 0 + 3) / 7 = 36.
    std::uint8_t block[16] = {255, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    std::uint8_t texels[64];
    decode_bc5_block(block, texels);
    for (int i = 0; i < 16; ++i) {
        CHECK(texels[4 * i + 0] == 36);
        CHECK(texels[4 * i + 1] == 0);
        CHECK(texels[4 * i + 2] == 0);
        CHECK(texels[4 * i + 3] == 255);
    }

    // Six-value mode with red0 = 0 and red1 = 255, every texel at index 2: (4 * 0 + 1 * 255 + 2) / 5 = 51.
    std::uint8_t six[16] = {0, 255, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49, 0, 0, 0, 0, 0, 0, 0, 0};
    decode_bc5_block(six, texels);
    CHECK(texels[0] == 51);

    std::uint8_t source[64];
    for (int i = 0; i < 16; ++i) {
        source[4 * i + 0] = static_cast<std::uint8_t>(i * 17);
        source[4 * i + 1] = static_cast<std::uint8_t>(255 - i * 17);
        source[4 * i + 2] = 0;
        source[4 * i + 3] = 255;
    }
    std::uint8_t encoded[16];
    encode_bc5_block(source, encoded);
    decode_bc5_block(encoded, texels);
    for (int i = 0; i < 16; ++i) {
        CHECK(std::abs(texels[4 * i + 0] - source[4 * i + 0]) <= 17);
        CHECK(std::abs(texels[4 * i + 1] - source[4 * i + 1]) <= 17);
    }

    std::uint8_t flat[64];
    for (int i = 0; i < 16; ++i) {
        flat[4 * i + 0] = 77;
        flat[4 * i + 1] = 200;
        flat[4 * i + 2] = 0;
        flat[4 * i + 3] = 255;
    }
    encode_bc5_block(flat, encoded);
    decode_bc5_block(encoded, texels);
    for (int i = 0; i < 16; ++i) {
        CHECK(texels[4 * i + 0] == 77);
        CHECK(texels[4 * i + 1] == 200);
    }
}

TEST_CASE("BC7 codec: a flat color block decodes exactly, and a gradient block decodes close to its source", "[unit]") {
    std::uint8_t flat[64];
    for (int i = 0; i < 16; ++i) {
        flat[4 * i + 0] = 200;
        flat[4 * i + 1] = 100;
        flat[4 * i + 2] = 50;
        flat[4 * i + 3] = 255;
    }
    std::uint8_t block[16];
    encode_bc7_block(flat, block, true);
    std::uint8_t texels[64];
    REQUIRE(decode_bc7_block(block, texels));
    for (int i = 0; i < 16; ++i) {
        CHECK(std::abs(texels[4 * i + 0] - 200) <= 2);
        CHECK(std::abs(texels[4 * i + 1] - 100) <= 2);
        CHECK(std::abs(texels[4 * i + 2] - 50) <= 2);
    }

    // The colors run along a line in RGB, which two endpoints represent, so the round trip is close.
    std::uint8_t gradient[64];
    for (int i = 0; i < 16; ++i) {
        gradient[4 * i + 0] = static_cast<std::uint8_t>((i % 4) * 85);
        gradient[4 * i + 1] = static_cast<std::uint8_t>(255 - (i % 4) * 85);
        gradient[4 * i + 2] = 128;
        gradient[4 * i + 3] = 255;
    }
    encode_bc7_block(gradient, block, false);
    REQUIRE(decode_bc7_block(block, texels));
    int total_error = 0;
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) {
            total_error += std::abs(texels[4 * i + c] - gradient[4 * i + c]);
        }
    }
    CHECK(static_cast<double>(total_error) / 48.0 <= 6.0);
}

TEST_CASE("Texture importer: settings default to color and auto, and unknown or malformed settings are rejected", "[unit]") {
    const auto importer = make_texture_importer();
    CHECK(importer->type() == "texture");

    auto defaults = importer->canonical_settings("{}");
    REQUIRE(defaults.ok());
    CHECK(defaults.value() == R"({"format":"auto","usage":"color"})");

    auto normal = importer->canonical_settings(R"({"usage":"normal"})");
    REQUIRE(normal.ok());
    CHECK(normal.value() == R"({"format":"auto","usage":"normal"})");

    CHECK_FALSE(importer->canonical_settings(R"({"usage":"bogus"})").ok());
    CHECK_FALSE(importer->canonical_settings(R"({"usage":3})").ok());
    CHECK_FALSE(importer->canonical_settings(R"({"format":"bc1"})").ok());
    CHECK_FALSE(importer->canonical_settings(R"({"mips":true})").ok());
    CHECK_FALSE(importer->canonical_settings("[]").ok());
    CHECK_FALSE(importer->canonical_settings("not json").ok());
}

TEST_CASE("Texture import: color mips average in linear light, and linear data averages as stored", "[integration]") {
    const std::string source = texture_fixture("srgb_2x2.png");

    // Color, RGBA8 fallback: the mip-1 texel averages black and white in linear light. The result is 188 in sRGB, while
    // averaging the encoded values would give 128.
    const TexturePayload color = read_payload(import_texture(source, R"({"format":"rgba8","usage":"color"})").payload);
    CHECK(color.srgb);
    CHECK(color.encoding == TextureEncoding::Rgba8);
    REQUIRE(color.mips.size() == 2);
    CHECK(byte_at(color.mips[0].data, 0) == 0);
    CHECK(byte_at(color.mips[0].data, 4) == 255);
    CHECK(byte_at(color.mips[1].data, 0) == 188);
    CHECK(byte_at(color.mips[1].data, 1) == 188);
    CHECK(byte_at(color.mips[1].data, 2) == 188);
    CHECK(byte_at(color.mips[1].data, 3) == 255);

    // Linear data is averaged as stored: the same two texels give 128.
    const TexturePayload linear = read_payload(import_texture(source, R"({"format":"rgba8","usage":"linear"})").payload);
    CHECK_FALSE(linear.srgb);
    CHECK(byte_at(linear.mips[1].data, 0) == 128);
}

TEST_CASE("Texture import: normal maps are renormalized, and every mip stays unit length", "[integration]") {
    const std::string source = texture_fixture("normal_5x3.png");
    const TexturePayload normals = read_payload(import_texture(source, R"({"format":"rgba8","usage":"normal"})").payload);
    CHECK_FALSE(normals.srgb);
    REQUIRE(normals.mips.size() == 4);

    // The texel (4, 2) is (1, 1, 1) in the source. Renormalized, it is (1, 1, 1) / sqrt(3), which encodes to 201.
    const std::size_t corner = 2 * 5 + 4;
    CHECK(byte_at(normals.mips[0].data, 4 * corner + 0) == 201);
    CHECK(byte_at(normals.mips[0].data, 4 * corner + 1) == 201);
    CHECK(byte_at(normals.mips[0].data, 4 * corner + 2) == 201);

    for (const TextureMip& mip : normals.mips) {
        const std::size_t texels = static_cast<std::size_t>(mip.width) * mip.height;
        for (std::size_t texel = 0; texel < texels; ++texel) {
            CHECK(std::abs(normal_length(mip.data, texel) - 1.0f) <= 0.02f);
        }
    }
}

TEST_CASE("Texture import: BC5 normals cook at the size of every mip for an odd source and decode close to it", "[integration]") {
    const std::string source = texture_fixture("normal_5x3.png");
    const TexturePayload normals = read_payload(import_texture(source, R"({"usage":"normal"})").payload);
    CHECK(normals.usage == TextureUsage::Normal);
    CHECK(normals.encoding == TextureEncoding::Bc5);
    REQUIRE(normals.mips.size() == 4);

    const std::uint32_t expected[4][2] = {{5, 3}, {3, 2}, {2, 1}, {1, 1}};
    const std::size_t expected_bytes[4] = {32, 16, 16, 16};
    for (std::size_t level = 0; level < 4; ++level) {
        CHECK(normals.mips[level].width == expected[level][0]);
        CHECK(normals.mips[level].height == expected[level][1]);
        CHECK(normals.mips[level].data.size() == expected_bytes[level]);
    }

    // The texel (4, 2) lies in the second block across, at block position (0, 2). It decodes within a few units of 201.
    std::uint8_t block[16];
    for (std::size_t i = 0; i < 16; ++i) {
        block[i] = byte_at(normals.mips[0].data, 16 + i);
    }
    std::uint8_t texels[64];
    decode_bc5_block(block, texels);
    CHECK(std::abs(texels[4 * (2 * 4 + 0) + 0] - 201) <= 4);
    CHECK(std::abs(texels[4 * (2 * 4 + 0) + 1] - 201) <= 4);
}

TEST_CASE("Texture import: BC7 color cooks to one block per mip and decodes close to the source", "[integration]") {
    const std::string source = texture_fixture("gradient_4x4.png");
    const TexturePayload color = read_payload(import_texture(source).payload);
    CHECK(color.encoding == TextureEncoding::Bc7);
    CHECK(color.srgb);
    REQUIRE(color.mips.size() == 3);
    CHECK(color.mips[0].data.size() == 16);
    CHECK(color.mips[1].data.size() == 16);
    CHECK(color.mips[2].data.size() == 16);

    std::uint8_t texels[64];
    REQUIRE(decode_bc7_block(reinterpret_cast<const std::uint8_t*>(color.mips[0].data.data()), texels));
    int total_error = 0;
    for (int i = 0; i < 16; ++i) {
        const int x = i % 4;
        const int expected_r = x * 85;
        const int expected_g = 255 - x * 85;
        total_error += std::abs(texels[4 * i + 0] - expected_r);
        total_error += std::abs(texels[4 * i + 1] - expected_g);
    }
    CHECK(static_cast<double>(total_error) / 32.0 <= 8.0);
}

TEST_CASE("Texture import: a 9x7 JPEG cooks every mip at its odd size, and a 16-bit PNG is reported", "[integration]") {
    const std::string jpeg = texture_fixture("gradient_9x7.jpg");
    const TexturePayload photo = read_payload(import_texture(jpeg).payload);
    REQUIRE(photo.mips.size() == 5);
    const std::uint32_t expected[5][2] = {{9, 7}, {5, 4}, {3, 2}, {2, 1}, {1, 1}};
    const std::size_t expected_bytes[5] = {96, 32, 16, 16, 16};
    for (std::size_t level = 0; level < 5; ++level) {
        CHECK(photo.mips[level].width == expected[level][0]);
        CHECK(photo.mips[level].height == expected[level][1]);
        CHECK(photo.mips[level].data.size() == expected_bytes[level]);
    }

    const ImportOutput deep = import_texture(texture_fixture("sixteen_bit_2x2.png"), R"({"format":"rgba8"})");
    REQUIRE(deep.unsupported.size() == 1);
    CHECK(deep.unsupported.front() == "16-bit samples were reduced to 8 bits");
    const TexturePayload reduced = read_payload(deep.payload);
    // The first texel is (0, 0x8000, 0xFFFF), which reduces to (0, 128, 255) in 8 bits.
    CHECK(byte_at(reduced.mips[0].data, 0) == 0);
    CHECK(std::abs(byte_at(reduced.mips[0].data, 1) - 128) <= 1);
    CHECK(byte_at(reduced.mips[0].data, 2) == 255);
}

TEST_CASE("Texture import: truncated, corrupt, and unrecognized files are rejected with a message", "[integration]") {
    const auto importer = make_texture_importer();
    const std::string settings = importer->canonical_settings("{}").value();
    const std::string png = texture_fixture("srgb_2x2.png");
    const std::string jpeg = texture_fixture("gradient_9x7.jpg");

    const auto rejects = [&](const std::string& source) {
        auto imported = importer->import(source, settings);
        return !imported.ok() && !imported.error().empty();
    };

    CHECK(rejects(""));
    CHECK(rejects("not an image at all"));
    CHECK(rejects(png.substr(0, png.size() - 12)));  // the IEND chunk is missing
    CHECK(rejects(png.substr(0, png.size() / 2)));
    CHECK(rejects(jpeg.substr(0, jpeg.size() - 2)));  // the end-of-image marker is missing

    // Every size of the PNG and JPEG is a valid input or a clean rejection, never a crash.
    for (std::size_t cut = 0; cut < png.size(); ++cut) {
        const auto result = importer->import(png.substr(0, cut), settings);
        CHECK((result.ok() || !result.error().empty()));
    }
}

TEST_CASE("Texture import: identical sources and settings produce identical payload bytes", "[integration]") {
    const std::string source = texture_fixture("gradient_4x4.png");
    const std::string settings = R"({"usage":"color"})";
    CHECK(import_texture(source, settings).payload == import_texture(source, settings).payload);
}

TEST_CASE("Texture importer: the built-in registry includes texture", "[unit]") {
    const auto registry = engine::converter::ImporterRegistry::builtin();
    const auto* importer = registry.find("texture");
    REQUIRE(importer != nullptr);
    CHECK(importer->version() >= 1);
}

TEST_CASE("Texture cook: a PNG cooks through the converter into an OWCK container that holds the texture payload", "[integration]") {
    const fs::path root = fs::temp_directory_path() / ("engine_texture_cook_" + Guid::generate().to_string());
    const fs::path project = root / "project";
    const fs::path output = root / "out";
    fs::create_directories(project / "textures");
    const std::string source = texture_fixture("gradient_4x4.png");
    {
        std::ofstream(project / "textures" / "gradient.png", std::ios::binary) << source;
        std::ofstream meta(project / "textures" / "gradient.png.meta", std::ios::binary);
        meta << R"({
  "format": "engine.asset_meta",
  "version": 1,
  "guid": "7d3e9a10-2b4c-4e6f-8a1b-0c2d3e4f5a61",
  "importer": {
    "type": "texture",
    "settings": {"usage": "color"}
  },
  "dependencies": []
})";
    }

    auto result = engine::converter::run(Command::Cook, Options{Path::from_utf8(utf8(project)).value(),
                                                               Path::from_utf8(utf8(output)).value(), 0},
                                         engine::converter::ImporterRegistry::builtin());
    INFO("convert failed: " << (result.ok() ? "" : result.error()));
    REQUIRE(result.ok());
    const auto report = std::move(result).value();
    CHECK(report.ok());
    REQUIRE(report.assets.size() == 1);
    CHECK(report.assets.front().status == AssetStatus::Cooked);

    const std::string cooked = read_bytes(output / "7d3e9a10-2b4c-4e6f-8a1b-0c2d3e4f5a61.cooked");
    REQUIRE(cooked.size() > kContainerHeaderBytes);
    CHECK(cooked.substr(0, 4) == "OWCK");
    const TexturePayload texture = read_payload(cooked.substr(kContainerHeaderBytes));
    CHECK(texture.encoding == TextureEncoding::Bc7);
    CHECK(texture.srgb);
    CHECK(texture.mips.size() == 3);

    fs::remove_all(root);
}
