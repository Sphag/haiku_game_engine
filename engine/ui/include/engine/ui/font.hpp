#pragma once

// Text for the game UI (US-058): a TrueType font (Roboto in the fixtures covers Cyrillic), UTF-8 decoding, and glyph
// rasterization into a cache. Text is laid out on one line; invalid UTF-8 bytes draw U+FFFD.

#include "engine/core/result.hpp"
#include "engine/ui/geometry.hpp"
#include "engine/ui/surface.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ui {

enum class UiError : std::uint8_t {
    InvalidFont,       // the bytes are not a usable TrueType or OpenType font
    UnknownElement,    // the id names no live element
    DuplicateImage,    // an image name is already registered
    UnknownImage,      // an element names an image that is not registered
    InvalidParent,     // the parent is not a live element, or is not a container
    InvalidArgument,   // a size, range, or other value is out of bounds
};

// Decodes the code point at `pos` in `text`, advancing `pos`. Invalid or truncated sequences decode as U+FFFD and advance one byte.
[[nodiscard]] char32_t decode_utf8(std::string_view text, std::size_t& pos) noexcept;

// Encodes one code point as UTF-8.
[[nodiscard]] std::string encode_utf8(char32_t codepoint);

class Font {
public:
    struct Metrics {
        float ascent = 0.0f;   // above the baseline
        float descent = 0.0f;  // below the baseline, positive
        float line_gap = 0.0f;
    };

    // Copies the bytes. Fails with InvalidFont when they are not a TrueType or OpenType font.
    [[nodiscard]] static core::Result<Font, UiError> load(const std::vector<std::uint8_t>& bytes);

    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    [[nodiscard]] Metrics metrics(float px) const;

    // Advance of a line, in pixels, including kerning. Each code point is measured on its own, so a line's width is
    // the sum of its pen advances.
    [[nodiscard]] float measure(std::string_view utf8, float px) const;

    // Draws one line with its pen starting at (x, baseline), clipped to `clip`, in pixels.
    void draw(Surface& target, std::string_view utf8, float px, float x, float baseline, Rgba8 colour, PixelRect clip) const;

    // Whether the font has a glyph for the code point (not counting the .notdef glyph).
    [[nodiscard]] bool has_glyph(char32_t codepoint) const;

private:
    struct Impl;
    explicit Font(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine::ui
