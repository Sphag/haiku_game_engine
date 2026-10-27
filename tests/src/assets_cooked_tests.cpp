// Cooked container and fallback texture (US-024): the layout the converter writes and the runtime reads, and the rules a
// damaged or foreign file must fail. These are pure byte checks, so they run without files or threads.

#include "engine/assets/cooked_container.hpp"
#include "engine/assets/texture_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using engine::assets::fallback_texture;
using engine::assets::kCookedContainerHeaderBytes;
using engine::assets::read_cooked_container;
using engine::assets::validate_texture;
using engine::assets::write_cooked_container;

namespace {

// Rewrites one little-endian field of a container so a test can damage exactly that field.
std::string with_field(std::string bytes, std::size_t offset, std::uint64_t value, int width) {
    for (int i = 0; i < width; ++i) {
        bytes[offset + static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xFFu);
    }
    return bytes;
}

}  // namespace

TEST_CASE("Cooked container: the layout is magic, version, payload size, then the payload", "[unit]") {
    const std::string container = write_cooked_container("abc");

    REQUIRE(container.size() == kCookedContainerHeaderBytes + 3);
    CHECK(container.substr(0, 4) == "OWCK");
    CHECK(container.substr(4, 4) == std::string("\x01\x00\x00\x00", 4));
    CHECK(container.substr(8, 8) == std::string("\x03\x00\x00\x00\x00\x00\x00\x00", 8));
    CHECK(container.substr(kCookedContainerHeaderBytes) == "abc");
}

TEST_CASE("Cooked container: a payload reads back unchanged, including an empty one", "[unit]") {
    for (const auto& payload : {std::string{}, std::string("text\0with nul", 13), std::string(4096, '\x7f')}) {
        const std::string container = write_cooked_container(payload);
        auto read = read_cooked_container(container);
        INFO("payload size " << payload.size() << ": " << (read.ok() ? "" : read.error()));
        REQUIRE(read.ok());
        CHECK(std::string(read.value()) == payload);
    }
}

TEST_CASE("Cooked container: a truncated header, a bad magic, and an unknown version are rejected", "[unit]") {
    const std::string good = write_cooked_container("payload");

    SECTION("a header cut short") {
        auto read = read_cooked_container(good.substr(0, kCookedContainerHeaderBytes - 1));
        REQUIRE_FALSE(read.ok());
        CHECK(read.error().find("truncated") != std::string::npos);
    }
    SECTION("a file that is not a container") {
        auto read = read_cooked_container("OWTX" + good.substr(4));
        REQUIRE_FALSE(read.ok());
        CHECK(read.error().find("magic") != std::string::npos);
    }
    SECTION("a version this runtime does not read") {
        auto read = read_cooked_container(with_field(good, 4, 2, 4));
        REQUIRE_FALSE(read.ok());
        CHECK(read.error().find("version 2") != std::string::npos);
    }
}

TEST_CASE("Cooked container: a payload size that disagrees with the file is rejected", "[unit]") {
    const std::string good = write_cooked_container("payload");

    SECTION("the file was cut short") {
        auto read = read_cooked_container(good.substr(0, good.size() - 1));
        REQUIRE_FALSE(read.ok());
        CHECK(read.error().find("does not match") != std::string::npos);
    }
    SECTION("bytes were appended after the payload") {
        auto read = read_cooked_container(good + "x");
        REQUIRE_FALSE(read.ok());
        CHECK(read.error().find("does not match") != std::string::npos);
    }
    SECTION("the size field claims more than the file holds") {
        auto read = read_cooked_container(with_field(good, 8, std::uint64_t{1} << 40, 8));
        REQUIRE_FALSE(read.ok());
        CHECK(read.error().find("does not match") != std::string::npos);
    }
}

TEST_CASE("Fallback texture: the handle fallback is a valid 1x1 texture that round-trips", "[unit]") {
    const auto texture = fallback_texture();

    auto valid = validate_texture(texture);
    INFO("fallback rejected: " << (valid.ok() ? "" : valid.error()));
    REQUIRE(valid.ok());
    REQUIRE(texture.mips.size() == 1);
    CHECK(texture.mips.front().width == 1);
    CHECK(texture.mips.front().height == 1);
    CHECK(texture.mips.front().data == std::string("\xFF\x00\xFF\xFF", 4));
}
