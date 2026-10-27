/* C shim over the vendored stb_truetype (US-058). Plain C ABI, so the first-party C++ never includes the vendor header.
 * Sizes are pixel heights; every function is safe to call on an open font and returns zero values for absent glyphs. */
#ifndef ENGINE_UI_TRUETYPE_SHIM_H
#define ENGINE_UI_TRUETYPE_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_truetype ui_truetype;

/* Copies the bytes and parses the first font in them. NULL when the bytes are not a TrueType/OpenType font. */
ui_truetype* ui_truetype_open(const uint8_t* data, size_t size);
void ui_truetype_close(ui_truetype* font);

/* Glyph index of a code point, 0 when the font has none. */
int ui_truetype_glyph(const ui_truetype* font, uint32_t codepoint);

/* Ascent (positive, above the baseline), descent (positive, below it), and line gap, in pixels at this size. */
void ui_truetype_vmetrics(const ui_truetype* font, float px, float* ascent, float* descent, float* line_gap);
float ui_truetype_advance(const ui_truetype* font, int glyph, float px);
float ui_truetype_kern(const ui_truetype* font, int first, int second, float px);

/* Coverage bitmap of one glyph at this size, with its offset from the pen position (left, and top relative to the baseline).
 * NULL for an empty glyph; release a non-NULL bitmap with ui_truetype_free_bitmap. */
uint8_t* ui_truetype_rasterize(const ui_truetype* font, int glyph, float px, int* width, int* height, int* left, int* top);
void ui_truetype_free_bitmap(uint8_t* bitmap);

#ifdef __cplusplus
}
#endif

#endif
