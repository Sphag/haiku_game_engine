// Game UI text (US-058): UTF-8 decoding, the Roboto font (Cyrillic included), measurement, and the rasterized output. These run on the
// CPU; the font is the vendored Roboto Regular, which covers Cyrillic.

#include "engine/ui/font.hpp"
#include "engine/ui/surface.hpp"
#include "engine/ui/ui.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using engine::ui::Font;
using engine::ui::Rgba8;
using engine::ui::Surface;
using engine::ui::Ui;

namespace {

std::vector<std::uint8_t> read_test_font() {
    std::ifstream file(ENGINE_TEST_FONT_PATH, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

Font load_test_font() {
    auto font = Font::load(read_test_font());
    REQUIRE(font.ok());
    return std::move(font).value();
}

// Decodes a whole string, returning the code points.
std::vector<char32_t> decode_all(const std::string& text) {
    std::vector<char32_t> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        out.push_back(engine::ui::decode_utf8(text, pos));
    }
    return out;
}

}  // namespace

TEST_CASE("UTF-8 decodes Cyrillic and rejects malformed sequences as U+FFFD", "[unit]") {
    // "Привет": П (U+041F), р (U+0440), и (U+0438), в (U+0432), е (U+0435), т (U+0442).
    CHECK(decode_all("Привет") == std::vector<char32_t>{0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442});
    CHECK(engine::ui::encode_utf8(0x041F) == "\xD0\x9F");
    CHECK(engine::ui::encode_utf8(0x1F600) == "\xF0\x9F\x98\x80");

    const char32_t replacement = 0xFFFD;
    CHECK(decode_all("\xC0\x80") == std::vector<char32_t>{replacement, replacement});  // overlong NUL
    CHECK(decode_all("\xED\xA0\x80") == std::vector<char32_t>{replacement, replacement, replacement});  // surrogate U+D800
    CHECK(decode_all("\xF4\x90\x80\x80") == std::vector<char32_t>(4, replacement));  // above U+10FFFF
    CHECK(decode_all("a\xD0") == std::vector<char32_t>{U'a', replacement});  // truncated at the end
    CHECK(decode_all("\xD0" "a") == std::vector<char32_t>{replacement, U'a'});  // missing continuation byte
}

TEST_CASE("Font loading rejects bytes that are not a TrueType font", "[unit]") {
    const auto empty = Font::load({});
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error() == engine::ui::UiError::InvalidFont);

    const std::vector<std::uint8_t> junk(64, 0x5A);
    const auto not_a_font = Font::load(junk);
    REQUIRE_FALSE(not_a_font.ok());
    CHECK(not_a_font.error() == engine::ui::UiError::InvalidFont);
}

TEST_CASE("Roboto covers Cyrillic and measures a line by its glyphs", "[unit]") {
    const Font font = load_test_font();
    CHECK(font.has_glyph(U'П'));
    CHECK(font.has_glyph(U'ё'));
    CHECK_FALSE(font.has_glyph(0x10FFFF));

    const float single = font.measure("П", 32.0f);
    const float word = font.measure("Привет", 32.0f);
    CHECK(single > 0.0f);
    CHECK(word > single * 4.0f);
    CHECK(font.measure("", 32.0f) == 0.0f);
    // Measurement scales with the pixel size.
    CHECK(font.measure("Привет", 64.0f) > word * 1.9f);
}

TEST_CASE("Drawn Cyrillic text covers pixels and stays inside its clip", "[unit]") {
    const Font font = load_test_font();
    Surface target(200, 80);
    const engine::ui::PixelRect clip{0, 0, 200, 80};
    font.draw(target, "Привет, мир", 32.0f, 10.0f, 50.0f, Rgba8{255, 0, 0, 255}, clip);

    std::uint32_t covered = 0;
    for (std::uint32_t y = 0; y < target.height(); ++y) {
        for (std::uint32_t x = 0; x < target.width(); ++x) {
            if (target.at(x, y).a != 0) {
                ++covered;
            }
        }
    }
    CHECK(covered > 200u);

    // A clip of half the surface keeps every drawn pixel inside it.
    Surface clipped(200, 80);
    font.draw(clipped, "Привет, мир", 32.0f, 10.0f, 50.0f, Rgba8{255, 0, 0, 255}, engine::ui::PixelRect{0, 0, 100, 80});
    for (std::uint32_t y = 0; y < clipped.height(); ++y) {
        for (std::uint32_t x = 100; x < clipped.width(); ++x) {
            CHECK(clipped.at(x, y).a == 0);
        }
    }
}

TEST_CASE("Text in a button renders at each window size with the layout scale", "[unit]") {
    const std::uint32_t sizes[][2] = {{640, 360}, {1280, 720}, {1920, 1080}};
    for (const auto& size : sizes) {
        Ui ui;
        ui.set_font(load_test_font());
        ui.set_viewport(size[0], size[1]);
        engine::ui::ElementSpec button;
        button.kind = engine::ui::ElementKind::Button;
        button.placement = engine::ui::place(engine::ui::Anchor::Centre, -120.0f, -30.0f, 240.0f, 60.0f);
        button.colour = Rgba8{0, 0, 255, 255};
        button.text_colour = Rgba8{255, 255, 255, 255};
        button.label = "Старт";
        button.text_size = 28.0f;
        REQUIRE(ui.add(engine::ui::kNoElement, button).ok());

        const Surface image = ui.render();
        REQUIRE(image.width() == size[0]);
        REQUIRE(image.height() == size[1]);
        // The button fills the scaled rectangle, so its interior centre is blue and its outside corner is empty.
        const float scale = ui.scale();
        const auto inside_x = static_cast<std::uint32_t>(static_cast<float>(size[0]) / 2.0f - 110.0f * scale);
        const auto inside_y = static_cast<std::uint32_t>(static_cast<float>(size[1]) / 2.0f - 25.0f * scale);
        CHECK(image.at(inside_x, inside_y).b == 255);
        CHECK(image.at(0, 0).a == 0);
    }
}
