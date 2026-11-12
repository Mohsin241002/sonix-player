#include "indic.h"
#include "src/system/core/respath.h"
#include "lvgl/src/font/freetype/lv_freetype_private.h"
#include <hb.h>
#include <hb-ot.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOKEN_BASE 0xF0000u
#define RAW_GLYPH 0x80000000u
#define GLYPHS_MAX 4096u
#define MEMORY_MAX (2u * 1024u * 1024u)
#define FONT_DIR SONIX_RESOURCE_DIR "/fonts/indic"

typedef struct {
    hb_script_t script;
    const char *file;
    hb_font_t *hb;
    lv_font_t *font;
    int pixels;
} face_t;

static face_t faces[] = {
    {HB_SCRIPT_DEVANAGARI, "NotoSansDevanagari-Regular.ttf"},
    {HB_SCRIPT_BENGALI, "NotoSansBengali-Regular.ttf"},
    {HB_SCRIPT_GURMUKHI, "NotoSansGurmukhi-Regular.ttf"},
    {HB_SCRIPT_GUJARATI, "NotoSansGujarati-Regular.ttf"},
    {HB_SCRIPT_ORIYA, "NotoSansOriya-Regular.ttf"},
    {HB_SCRIPT_TAMIL, "NotoSansTamil-Regular.ttf"},
    {HB_SCRIPT_TELUGU, "NotoSansTelugu-Regular.ttf"},
    {HB_SCRIPT_KANNADA, "NotoSansKannada-Regular.ttf"},
    {HB_SCRIPT_MALAYALAM, "NotoSansMalayalam-Regular.ttf"},
    {HB_SCRIPT_OL_CHIKI, "NotoSansOlChiki-Regular.ttf"},
    {HB_SCRIPT_MEETEI_MAYEK, "NotoSansMeeteiMayek-Regular.ttf"},
};
#define FACE_COUNT (sizeof(faces) / sizeof(faces[0]))

typedef struct { void *data; size_t size; } mapping_t;
typedef struct {
    uint16_t gid, advance, width, height;
    int16_t x, y;
    uint8_t face;
} glyph_t;
typedef struct { uint32_t letter; uint16_t width; bool cluster; } token_t;
typedef struct {
    lv_font_t font;
    const lv_font_t *base;
    glyph_t *glyphs;
    unsigned count, capacity;
    size_t bytes;
} shaped_t;

static size_t memory_used;
static unsigned live_labels;
size_t indic_memory_bytes(void) { return memory_used; }

static void mapping_free(void *user) {
    mapping_t *m = user;
    munmap(m->data, m->size);
    free(m);
}

static void faces_close(void) {
    if (live_labels) return;
    for (size_t i = 0; i < FACE_COUNT; i++) {
        if (faces[i].hb) hb_font_destroy(faces[i].hb);
        if (faces[i].font) lv_freetype_font_delete(faces[i].font);
        faces[i].hb = NULL;
        faces[i].font = NULL;
        faces[i].pixels = 0;
    }
}

static face_t *face_open(unsigned i, int pixels) {
    face_t *f = &faces[i];
    if (f->hb) return f->pixels == pixels ? f : NULL;
    const char *dir = FONT_DIR;
#ifdef HOST_BUILD
    const char *override = getenv("SONIX_INDIC_FONT_DIR");
    if (override && override[0]) dir = override;
#endif
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", dir, f->file) >= (int)sizeof(path)) return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) { close(fd); return NULL; }
    mapping_t *m = malloc(sizeof(*m));
    if (!m) { close(fd); return NULL; }
    m->size = (size_t)st.st_size;
    m->data = mmap(NULL, m->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m->data == MAP_FAILED) { free(m); return NULL; }
    hb_blob_t *blob = hb_blob_create(m->data, (unsigned)m->size, HB_MEMORY_MODE_READONLY, m, mapping_free);
    hb_face_t *hb_face = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    if (!hb_face_get_glyph_count(hb_face)) { hb_face_destroy(hb_face); return NULL; }
    f->font = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, pixels, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (!f->font) { hb_face_destroy(hb_face); return NULL; }
    f->hb = hb_font_create(hb_face);
    hb_face_destroy(hb_face);
    hb_ot_font_set_funcs(f->hb);
    hb_font_set_scale(f->hb, pixels * 64, pixels * 64);
    f->pixels = pixels;
    return f;
}

static uint32_t next_char(const char **at) {
    const unsigned char *p = (const unsigned char *)*at;
    unsigned n = 1; uint32_t c = *p;
    if (c >= 0xC2 && c <= 0xDF) { n = 2; c &= 31; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; c &= 15; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; c &= 7; }
    else if (c >= 0x80) { (*at)++; return 0xFFFD; }
    for (unsigned i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { (*at)++; return 0xFFFD; }
        c = (c << 6) | (p[i] & 63);
    }
    *at += n;
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) || (n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000)) return 0xFFFD;
    return c;
}

static char *put_char(char *p, uint32_t c) {
    if (c < 0x80) *p++ = (char)c;
    else if (c < 0x800) { *p++ = 0xC0 | (c >> 6); *p++ = 0x80 | (c & 63); }
    else if (c < 0x10000) { *p++ = 0xE0 | (c >> 12); *p++ = 0x80 | ((c >> 6) & 63); *p++ = 0x80 | (c & 63); }
    else { *p++ = 0xF0 | (c >> 18); *p++ = 0x80 | ((c >> 12) & 63); *p++ = 0x80 | ((c >> 6) & 63); *p++ = 0x80 | (c & 63); }
    return p;
}

static int script_for(uint32_t c) {
    hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), c);
    for (unsigned i = 0; i < FACE_COUNT; i++) if (faces[i].script == script) return (int)i;
    return -1;
}

static int rounded(int x) { return x >= 0 ? (x + 32) / 64 : -((-x + 32) / 64); }

static bool shape_run(shaped_t *s, unsigned face, int pixels, const char *text, size_t length, token_t *tokens, unsigned *n) {
    face_t *f = face_open(face, pixels);
    if (!f) return false;
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text, (int)length, 0, (int)length);
    hb_buffer_set_script(buffer, f->script);
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(f->hb, buffer, NULL, 0);
    unsigned count = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, NULL);
    if (!hb_buffer_allocation_successful(buffer) || !count || count > GLYPHS_MAX - s->count || count > GLYPHS_MAX - *n) { hb_buffer_destroy(buffer); return false; }
    if (s->count + count > s->capacity) {
        unsigned capacity = s->count + count;
        glyph_t *more = realloc(s->glyphs, capacity * sizeof(*more));
        if (!more) { hb_buffer_destroy(buffer); return false; }
        s->glyphs = more; s->capacity = capacity;
    }
    lv_freetype_font_dsc_t *ft = (lv_freetype_font_dsc_t *)f->font->dsc;
    lv_mutex_lock(&ft->cache_node->face_lock);
    FT_Face raw = ft->cache_node->face;
    bool ok = FT_Set_Pixel_Sizes(raw, 0, pixels) == 0;
    int pen = 0;
    for (unsigned i = 0; ok && i < count; i++) {
        if (!info[i].codepoint || info[i].codepoint > UINT16_MAX || FT_Load_Glyph(raw, info[i].codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_AUTOHINT)) { ok = false; break; }
        FT_GlyphSlot slot = raw->glyph;
        int advance = rounded(pen + pos[i].x_advance) - rounded(pen);
        if (advance < 0 || advance > 1024 || slot->bitmap.width > 512 || slot->bitmap.rows > 512) { ok = false; break; }
        unsigned index = s->count++;
        glyph_t *g = &s->glyphs[index];
        *g = (glyph_t){info[i].codepoint, advance, slot->bitmap.width, slot->bitmap.rows,
            slot->bitmap_left + rounded(pen + pos[i].x_offset) - rounded(pen),
            slot->bitmap_top - (int)slot->bitmap.rows + rounded(pos[i].y_offset), face};
        tokens[(*n)++] = (token_t){TOKEN_BASE + index, advance, i == 0 || info[i].cluster != info[i - 1].cluster};
        pen += pos[i].x_advance;
        int ascent = s->font.line_height - s->font.base_line;
        int descent = s->font.base_line;
        if (g->y + g->height + 2 > ascent) ascent = g->y + g->height + 2;
        if (-g->y + 2 > descent) descent = -g->y + 2;
        s->font.line_height = ascent + descent;
        s->font.base_line = descent;
    }
    lv_mutex_unlock(&ft->cache_node->face_lock);
    hb_buffer_destroy(buffer);
    return ok;
}

// LVGL measures original Unicode or a private token carrying a positioned
// glyph. Bitmap acquisition/release is delegated to the real FreeType face,
// retaining its shared LRU cache instead of storing bitmaps per lyric line.
static bool glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *d, uint32_t c, uint32_t next) {
    (void)next;
    const shaped_t *s = font->dsc;
    if (c >= TOKEN_BASE && c < TOKEN_BASE + s->count) {
        unsigned i = c - TOKEN_BASE;
        const glyph_t *g = &s->glyphs[i];
        *d = (lv_font_glyph_dsc_t){.gid.index = i, .adv_w = g->advance, .box_w = g->width, .box_h = g->height,
            .ofs_x = g->x, .ofs_y = g->y, .format = LV_FONT_GLYPH_FORMAT_A8};
        return true;
    }
    if (!lv_font_get_glyph_dsc(s->base, d, c, 0)) return false;
    d->gid.index = RAW_GLYPH | c;
    return true;
}

static const lv_font_t *bitmap_font(const shaped_t *s, lv_font_glyph_dsc_t *d) {
    if (d->gid.index & RAW_GLYPH) {
        lv_font_glyph_dsc_t raw;
        if (!lv_font_get_glyph_dsc(s->base, &raw, d->gid.index & ~RAW_GLYPH, 0)) return NULL;
        d->gid.index = raw.gid.index;
        d->resolved_font = raw.resolved_font;
    } else {
        if (d->gid.index >= s->count) return NULL;
        const glyph_t *g = &s->glyphs[d->gid.index];
        d->gid.index = g->gid;
        d->resolved_font = faces[g->face].font;
    }
    return d->resolved_font;
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *d, lv_draw_buf_t *draw) {
    const shaped_t *s = d->resolved_font->dsc;
    lv_font_glyph_dsc_t real = *d;
    const lv_font_t *font = bitmap_font(s, &real);
    if (!font || !font->get_glyph_bitmap) return NULL;
    const void *result = font->get_glyph_bitmap(&real, draw);
    d->entry = real.entry;
    return result;
}

static void glyph_release(const lv_font_t *font, lv_font_glyph_dsc_t *d) {
    lv_font_glyph_dsc_t real = *d;
    const lv_font_t *original = bitmap_font(font->dsc, &real);
    if (original && original->release_glyph && real.entry) original->release_glyph(original, &real);
    d->entry = NULL;
}

static void label_deleted(lv_event_t *e) {
    shaped_t *s = lv_event_get_user_data(e);
    memory_used -= s->bytes;
    free(s->glyphs); free(s);
    live_labels--;
    faces_close();
}

bool indic_label_set_text(lv_obj_t *label, const char *text, const lv_font_t *base, int pixels, int width) {
    if (!label || !text || !base || pixels < 8 || pixels > 72 || width < 1 || strlen(text) > 16384) return false;
    bool has_indic = false;
    for (const char *p = text; *p;) {
        uint32_t c = next_char(&p);
        if (c >= TOKEN_BASE && c < TOKEN_BASE + GLYPHS_MAX) return false;
        if (script_for(c) >= 0) has_indic = true;
    }
    if (!has_indic) return false;
    shaped_t *s = calloc(1, sizeof(*s));
    token_t *tokens = malloc(GLYPHS_MAX * sizeof(*tokens));
    char *encoded = malloc(GLYPHS_MAX * 8u + 1u);
    if (!s || !tokens || !encoded) { free(s); free(tokens); free(encoded); return false; }
    s->base = base; s->font = *base;
    s->font.dsc = s; s->font.fallback = NULL; s->font.kerning = LV_FONT_KERNING_NONE;
    s->font.get_glyph_dsc = glyph_dsc; s->font.get_glyph_bitmap = glyph_bitmap; s->font.release_glyph = glyph_release;
    unsigned n = 0; bool ok = true;
    for (const char *p = text; *p && ok;) {
        const char *start = p;
        uint32_t c = next_char(&p);
        int face = script_for(c);
        if (face >= 0) {
            for (const char *q = p; *q;) {
                const char *before = q; uint32_t next = next_char(&q);
                hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), next);
                if (script_for(next) != face && next != 0x200C && next != 0x200D && script != HB_SCRIPT_INHERITED) { q = before; break; }
                p = q;
            }
            ok = shape_run(s, (unsigned)face, pixels, start, (size_t)(p - start), tokens, &n);
        } else {
            if (n >= GLYPHS_MAX) { ok = false; break; }
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
            lv_font_glyph_dsc_t d = {0};
            lv_font_get_glyph_dsc(base, &d, c, 0);
            tokens[n++] = (token_t){c, d.adv_w, true};
        }
    }
    char *out = encoded; int line_width = 0;
    // Wrap whole words, or whole HarfBuzz clusters when a word is too long.
    // Explicit line breaks with CLIP mode prevent LVGL breaking a conjunct.
    for (unsigned i = 0; ok && i < n;) {
        if (tokens[i].letter == '\n') { *out++ = '\n'; line_width = 0; i++; continue; }
        if (tokens[i].letter == ' ') {
            if (line_width && line_width + tokens[i].width <= width) { *out++ = ' '; line_width += tokens[i].width; }
            i++; continue;
        }
        unsigned end = i; int word_width = 0;
        while (end < n && tokens[end].letter != ' ' && tokens[end].letter != '\n') word_width += tokens[end++].width;
        if (line_width && line_width + word_width > width) { *out++ = '\n'; line_width = 0; }
        while (i < end) {
            unsigned cluster_end = i + 1; int cluster_width = tokens[i].width;
            while (cluster_end < end && !tokens[cluster_end].cluster) cluster_width += tokens[cluster_end++].width;
            if (line_width && line_width + cluster_width > width) { *out++ = '\n'; line_width = 0; }
            while (i < cluster_end) out = put_char(out, tokens[i++].letter);
            line_width += cluster_width;
        }
    }
    *out = '\0';
    s->bytes = sizeof(*s) + s->capacity * sizeof(*s->glyphs);
    if (s->bytes > MEMORY_MAX - memory_used) ok = false;
    free(tokens);
    if (!ok || !s->count) { free(encoded); free(s->glyphs); free(s); faces_close(); return false; }
    memory_used += s->bytes; live_labels++;
    lv_obj_set_style_text_font(label, &s->font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, encoded);
    lv_obj_add_event_cb(label, label_deleted, LV_EVENT_DELETE, s);
    free(encoded);
    return true;
}
