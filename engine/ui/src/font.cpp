#include "engine/ui/font.hpp"

#include "truetype_shim.h"

#include <cmath>
#include <cstddef>
#include <map>
#include <utility>

namespace engine::ui {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Pixel sizes are cached at 1/64 px, so a size that differs only by float noise shares its glyphs.
std::uint32_t size_key(float px) {
    return static_cast<std::uint32_t>(std::lround(px * 64.0f));
}

struct Glyph {
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    std::vector<std::uint8_t> coverage;
};

struct ShimDeleter {
    void operator()(ui_truetype* font) const noexcept {
        ui_truetype_close(font);
    }
};

}  // namespace

struct Font::Impl {
    std::unique_ptr<ui_truetype, ShimDeleter> shim;
    // Keyed by (code point, size key). Mutable state is the cache only; the font itself stays unchanged.
    mutable std::map<std::pair<std::uint32_t, std::uint32_t>, Glyph> cache;

    [[nodiscard]] const Glyph& glyph(char32_t codepoint, float px) const {
        const auto key = std::make_pair(static_cast<std::uint32_t>(codepoint), size_key(px));
        const auto found = cache.find(key);
        if (found != cache.end()) {
            return found->second;
        }
        Glyph entry;
        const int index = ui_truetype_glyph(shim.get(), static_cast<std::uint32_t>(codepoint));
        int width = 0;
        int height = 0;
        int left = 0;
        int top = 0;
        std::uint8_t* bitmap = ui_truetype_rasterize(shim.get(), index, px, &width, &height, &left, &top);
        if (bitmap != nullptr && width > 0 && height > 0) {
            entry.width = width;
            entry.height = height;
            entry.left = left;
            entry.top = top;
            const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            entry.coverage.assign(bitmap, bitmap + count);
        }
        ui_truetype_free_bitmap(bitmap);
        return cache.emplace(key, std::move(entry)).first->second;
    }
};

Font::Font(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Font::Font(Font&&) noexcept = default;
Font& Font::operator=(Font&&) noexcept = default;
Font::~Font() = default;

core::Result<Font, UiError> Font::load(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return core::failure(UiError::InvalidFont);
    }
    ui_truetype* shim = ui_truetype_open(bytes.data(), bytes.size());
    if (shim == nullptr) {
        return core::failure(UiError::InvalidFont);
    }
    auto impl = std::make_unique<Impl>();
    impl->shim.reset(shim);
    return core::success(Font(std::move(impl)));
}

Font::Metrics Font::metrics(float px) const {
    Metrics result;
    ui_truetype_vmetrics(impl_->shim.get(), px, &result.ascent, &result.descent, &result.line_gap);
    return result;
}

bool Font::has_glyph(char32_t codepoint) const {
    return ui_truetype_glyph(impl_->shim.get(), static_cast<std::uint32_t>(codepoint)) != 0;
}

float Font::measure(std::string_view utf8, float px) const {
    float width = 0.0f;
    int previous = -1;
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const char32_t codepoint = decode_utf8(utf8, pos);
        const int glyph = ui_truetype_glyph(impl_->shim.get(), static_cast<std::uint32_t>(codepoint));
        if (previous >= 0) {
            width += ui_truetype_kern(impl_->shim.get(), previous, glyph, px);
        }
        width += ui_truetype_advance(impl_->shim.get(), glyph, px);
        previous = glyph;
    }
    return width;
}

void Font::draw(Surface& target, std::string_view utf8, float px, float x, float baseline, Rgba8 colour, PixelRect clip) const {
    float pen = x;
    int previous = -1;
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const char32_t codepoint = decode_utf8(utf8, pos);
        const int glyph = ui_truetype_glyph(impl_->shim.get(), static_cast<std::uint32_t>(codepoint));
        if (previous >= 0) {
            pen += ui_truetype_kern(impl_->shim.get(), previous, glyph, px);
        }
        const Glyph& bitmap = impl_->glyph(codepoint, px);
        if (!bitmap.coverage.empty()) {
            const int left = static_cast<int>(std::lround(pen)) + bitmap.left;
            const int top = static_cast<int>(std::lround(baseline)) + bitmap.top;
            target.blit_coverage(left, top, bitmap.width, bitmap.height, bitmap.coverage.data(), colour, clip);
        }
        pen += ui_truetype_advance(impl_->shim.get(), glyph, px);
        previous = glyph;
    }
}

std::string encode_utf8(char32_t codepoint) {
    std::string out;
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return out;
}

char32_t decode_utf8(std::string_view text, std::size_t& pos) noexcept {
    const auto lead = static_cast<unsigned char>(text[pos]);
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (lead < 0x80) {
        ++pos;
        return static_cast<char32_t>(lead);
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = static_cast<char32_t>(lead & 0x1F);
        minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = static_cast<char32_t>(lead & 0x0F);
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = static_cast<char32_t>(lead & 0x07);
        minimum = 0x10000;
    } else {
        ++pos;
        return kReplacement;
    }
    if (pos + length > text.size()) {
        ++pos;
        return kReplacement;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[pos + i]);
        if ((next & 0xC0) != 0x80) {
            ++pos;
            return kReplacement;
        }
        value = (value << 6) | static_cast<char32_t>(next & 0x3F);
    }
    // Rejects overlong forms, surrogates, and values above U+10FFFF.
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        ++pos;
        return kReplacement;
    }
    pos += length;
    return value;
}

}  // namespace engine::ui
