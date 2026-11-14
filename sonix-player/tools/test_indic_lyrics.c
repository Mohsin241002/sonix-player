#include "lvgl/lvgl.h"
#include "src/gui/fonts/indic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

lv_font_t font_ui_14;
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(30);
    assert(argc == 2);
    puts("Indic test: starting LVGL");
    puts("Indic test: starting LVGL");
    lv_init();
    puts("Indic test: LVGL initialized");
    puts("Indic test: LVGL initialized");
    char path[2048];
    snprintf(path, sizeof(path), "%s/NotoSansDevanagari-Regular.ttf", getenv("SONIX_INDIC_FONT_DIR"));
    lv_font_t *base = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 26, LV_FREETYPE_FONT_STYLE_NORMAL);
    assert(base);
    puts("Indic test: base font opened");
    puts("Indic test: base font opened");
    font_ui_14 = *base;
    enum {W = 480, H = 1180};
    uint16_t *pixels = calloc(W * H, sizeof(*pixels));
    assert(pixels);
    lv_display_t *display = lv_display_create(W, H);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, pixels, NULL, W * H * 2, LV_DISPLAY_RENDER_MODE_FULL);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    static const struct { const char *name, *text; } samples[] = {
        {"Hindi / Marathi", "नमस्ते दुनिया कि क्ष त्र ज्ञ महाराष्ट्र"},
        {"Bengali / Assamese", "বাংলা ভাষা অসমীয়া ভাষা"},
        {"Punjabi", "ਸਤਿ ਸ੍ਰੀ ਅਕਾਲ ਪੰਜਾਬੀ"},
        {"Gujarati", "ગુજરાતી ભાષા નમસ્તે"},
        {"Odia", "ଓଡ଼ିଆ ଭାଷା ନମସ୍କାର"},
        {"Tamil", "தமிழ் மொழி வணக்கம்"},
        {"Telugu", "తెలుగు భాష నమస్కారం"},
        {"Kannada", "ಕನ್ನಡ ಭಾಷೆ ನಮಸ್ಕಾರ"},
        {"Malayalam", "മലയാളം ഭാഷ നമസ്കാരം"},
        {"Santali / Ol Chiki", "ᱥᱟᱱᱛᱟᱲᱤ ᱚᱞ ᱪᱤᱠᱤ"},
        {"Manipuri / Meetei Mayek", "ꯃꯩꯇꯩ ꯂꯣꯟ"},
        {"Mixed text", "Hello नमस्ते 2026 தமிழ் বাংলা"},
    };
    int y = 8;
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        printf("Shaping %s\n", samples[i].name);
        printf("Shaping %s\n", samples[i].name);
        lv_obj_t *name = lv_label_create(screen);
        lv_label_set_text(name, samples[i].name);
        lv_obj_set_style_text_font(name, base, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0x666666), 0);
        lv_obj_set_pos(name, 24, y);
        lv_obj_t *line = lv_label_create(screen);
        lv_obj_set_width(line, 432);
        lv_obj_set_pos(line, 24, y + 34);
        assert(indic_label_set_text(line, samples[i].text, base, 26, 432));
        lv_obj_set_style_text_color(line, lv_color_black(), 0);
        lv_obj_update_layout(line);
        y += 34 + lv_obj_get_height(line) + 10;
    }
    lv_refr_now(display);
    FILE *out = fopen(argv[1], "wb"); assert(out);
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t p = pixels[i];
        unsigned char rgb[] = {(p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
    printf("Indic preview: %d rows; shaped positions: %zu bytes\n", y, indic_memory_bytes());
    lv_obj_clean(screen);
    assert(indic_memory_bytes() == 0);
    lv_obj_t *test = lv_label_create(screen);
    lv_obj_set_width(test, 35);
    assert(indic_label_set_text(test, "क्षिक्षिक्षिक्षिक्षिक्षि", base, 26, 35));
    assert(strchr(lv_label_get_text(test), '\n'));
    lv_obj_update_layout(test);
    assert(lv_obj_get_height(test) > 40);
    lv_obj_delete(test);
    assert(indic_memory_bytes() == 0);
    // Active lyrics enlarge while neighboring lines retain their normal size.
    lv_obj_t *neighbor = lv_label_create(screen);
    assert(indic_label_set_text(neighbor, "नमस्ते दुनिया", base, 28, 432));
    test = lv_label_create(screen);
    size_t steady = 0;
    for (int i = 0; i < 100; i++) {
        int sizes[] = {28, 38, 32};
        assert(indic_label_set_text(test, "रंगी सारी गुलाबी चुनरिया रे", base, sizes[i % 3], 432));
        const lv_font_t *shaped = lv_obj_get_style_text_font(test, LV_PART_MAIN);
        lv_font_glyph_dsc_t glyph = {0};
        assert(lv_font_get_glyph_dsc(shaped, &glyph, 0xF0000, 0));
        assert(glyph.box_h > 0 && glyph.box_w > 0);
        lv_refr_now(display);
        if (i == 0) steady = indic_memory_bytes();
        assert(indic_memory_bytes() == steady);
    }
    lv_obj_delete(test);
    lv_obj_delete(neighbor);
    assert(indic_memory_bytes() == 0);
    // A normal lyric remains normal, and malformed input must not escape bounds.
    test = lv_label_create(screen);
    assert(!indic_label_set_text(test, "ordinary English", base, 26, 400));
    assert(!indic_label_set_text(test, "\xf0\x9f", base, 26, 400));
    lv_obj_delete(test);
    for (int i = 0; i < 100; i++) {
        test = lv_label_create(screen);
        assert(indic_label_set_text(test, "कि क्ष தமிழ் বাংলা", base, 26, 200));
        lv_obj_delete(test);
        assert(indic_memory_bytes() == 0);
    }
    lv_display_delete(display);
    lv_freetype_font_delete(base);
    lv_deinit();
    free(pixels);
    puts("Indic lyric shaping, wrapping, rendering and cleanup: PASS");
    return 0;
}
