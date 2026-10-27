#include "engine/core/utf8.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::core::is_valid_utf8;

TEST_CASE("ASCII, Cyrillic, and 4-byte sequences are valid UTF-8", "[unit]") {
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("plain ascii"));
    CHECK(is_valid_utf8("Привет, мир!"));
    CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));  // U+1F600
}

TEST_CASE("Malformed UTF-8 is rejected", "[unit]") {
    CHECK_FALSE(is_valid_utf8("\x80"));                  // lone continuation byte
    CHECK_FALSE(is_valid_utf8("\xC3\x28"));              // invalid continuation
    CHECK_FALSE(is_valid_utf8("\xC0\xAF"));              // overlong '/'
    CHECK_FALSE(is_valid_utf8("\xE0\x80\xAF"));          // overlong, 3 bytes
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));          // UTF-16 surrogate U+D800
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));      // above U+10FFFF
    CHECK_FALSE(is_valid_utf8("\xF5\x80\x80\x80"));      // invalid lead byte
    CHECK_FALSE(is_valid_utf8("\xE2\x82"));              // truncated sequence
}
