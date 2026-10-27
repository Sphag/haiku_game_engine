#include "engine/core/guid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

using engine::core::Guid;
using engine::core::GuidError;

TEST_CASE("A default GUID is nil", "[unit]") {
    const Guid guid;

    CHECK(guid.is_nil());
    CHECK(guid.to_string() == "00000000-0000-0000-0000-000000000000");
}

TEST_CASE("Generated GUIDs are version 4 with the RFC 4122 variant", "[unit]") {
    for (int i = 0; i < 100; ++i) {
        const auto guid = Guid::generate();
        const auto& bytes = guid.bytes();

        CHECK_FALSE(guid.is_nil());
        CHECK((bytes[6] >> 4) == 4);
        CHECK((bytes[8] >> 6) == 2);
    }
}

TEST_CASE("Generated GUIDs are unique across many draws", "[unit]") {
    std::unordered_set<Guid> seen;
    for (int i = 0; i < 10000; ++i) {
        CHECK(seen.insert(Guid::generate()).second);
    }
}

TEST_CASE("GUIDs format as lowercase canonical text and parse back", "[unit]") {
    const auto guid = Guid::generate();
    const auto text = guid.to_string();

    REQUIRE(text.size() == 36);
    CHECK(text[8] == '-');
    CHECK(text[13] == '-');
    CHECK(text[18] == '-');
    CHECK(text[23] == '-');

    const auto parsed = Guid::parse(text);
    REQUIRE(parsed.ok());
    CHECK(parsed.value() == guid);
}

TEST_CASE("GUID parsing accepts uppercase hex", "[unit]") {
    const auto parsed = Guid::parse("3F2504E0-4F89-41D3-9A0C-0305E82C3301");

    REQUIRE(parsed.ok());
    CHECK(parsed.value().to_string() == "3f2504e0-4f89-41d3-9a0c-0305e82c3301");
}

TEST_CASE("GUID parsing rejects malformed text", "[unit]") {
    CHECK_FALSE(Guid::parse("").ok());
    CHECK_FALSE(Guid::parse("3f2504e0-4f89-41d3-9a0c-0305e82c330").ok());
    CHECK_FALSE(Guid::parse("3f2504e04f89-41d3-9a0c-0305e82c3301-").ok());
    CHECK_FALSE(Guid::parse("3f2504e0-4f89-41d3-9a0c-0305e82c330g").ok());
    CHECK(Guid::parse("3f2504e0-4f89-41d3-9a0c-0305e82c330g").error() == GuidError::InvalidFormat);
}

TEST_CASE("GUIDs compare and hash by value", "[unit]") {
    const Guid::Bytes bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const Guid first(bytes);
    const Guid second(bytes);

    CHECK(first == second);
    CHECK(std::hash<Guid>{}(first) == std::hash<Guid>{}(second));
    CHECK(Guid() < first);
}
