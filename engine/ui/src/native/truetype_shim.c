/* stb_truetype behind the shim (US-058). The vendor implementation is compiled here as static code, so no other object file
 * sees its symbols. The vendor header is outside the first-party warning policy. */
#include "truetype_shim.h"

#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

struct ui_truetype {
    stbtt_fontinfo info;
    uint8_t* data;
    size_t size;
};

ui_truetype* ui_truetype_open(const uint8_t* data, size_t size) {
    ui_truetype* font;
    if (data == NULL || size == 0) {
        return NULL;
    }
    font = (ui_truetype*)malloc(sizeof(ui_truetype));
    if (font == NULL) {
        return NULL;
    }
    font->data = (uint8_t*)malloc(size);
    if (font->data == NULL) {
        free(font);
        return NULL;
    }
    memcpy(font->data, data, size);
    font->size = size;
    if (stbtt_GetNumberOfFonts(font->data) < 1 || !stbtt_InitFont(&font->info, font->data, stbtt_GetFontOffsetForIndex(font->data, 0))) {
        free(font->data);
        free(font);
        return NULL;
    }
    return font;
}

void ui_truetype_close(ui_truetype* font) {
    if (font == NULL) {
        return;
    }
    free(font->data);
    free(font);
}

int ui_truetype_glyph(const ui_truetype* font, uint32_t codepoint) {
    return stbtt_FindGlyphIndex(&font->info, (int)codepoint);
}

void ui_truetype_vmetrics(const ui_truetype* font, float px, float* ascent, float* descent, float* line_gap) {
    const float scale = stbtt_ScaleForPixelHeight(&font->info, px);
    int a = 0;
    int d = 0;
    int g = 0;
    stbtt_GetFontVMetrics(&font->info, &a, &d, &g);
    *ascent = (float)a * scale;
    *descent = -(float)d * scale;
    *line_gap = (float)g * scale;
}

float ui_truetype_advance(const ui_truetype* font, int glyph, float px) {
    int advance = 0;
    int left_side_bearing = 0;
    stbtt_GetGlyphHMetrics(&font->info, glyph, &advance, &left_side_bearing);
    return (float)advance * stbtt_ScaleForPixelHeight(&font->info, px);
}

float ui_truetype_kern(const ui_truetype* font, int first, int second, float px) {
    return (float)stbtt_GetGlyphKernAdvance(&font->info, first, second) * stbtt_ScaleForPixelHeight(&font->info, px);
}

uint8_t* ui_truetype_rasterize(const ui_truetype* font, int glyph, float px, int* width, int* height, int* left, int* top) {
    const float scale = stbtt_ScaleForPixelHeight(&font->info, px);
    *width = 0;
    *height = 0;
    *left = 0;
    *top = 0;
    return stbtt_GetGlyphBitmap(&font->info, scale, scale, glyph, width, height, left, top);
}

void ui_truetype_free_bitmap(uint8_t* bitmap) {
    stbtt_FreeBitmap(bitmap, NULL);
}
