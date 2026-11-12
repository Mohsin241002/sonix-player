#ifndef INDIC_H
#define INDIC_H

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stddef.h>

// Shape Indic lyric text once. The label owns its glyph positions until it is
// deleted; rasterized glyphs use LVGL's existing bounded FreeType cache.
// False leaves the label untouched, so ordinary Unicode labels remain usable.
bool indic_label_set_text(lv_obj_t *label, const char *text, const lv_font_t *base, int pixels, int width);
size_t indic_memory_bytes(void);

#endif
