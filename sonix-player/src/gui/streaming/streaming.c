#include "streaming.h"
#include "src/gui/shell/navbar.h"

#include "lvgl/lvgl.h"

#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/tidal.h"

lv_obj_t *streaming_screen;

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);

	const grid_entry_t entries[] = {
		{"radio", &icon_menu_radio, &radiopage_screen, NULL},
		{"podcasts", &icon_menu_podcast, &podcast_screen, NULL},
		{"tidal", &icon_menu_tidal, &tidal_screen, NULL},
		{"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL},
	};

	// Two columns and three rows of tile, exactly like Music and Wireless, so a
	// tile here is the same size as a tile there. Four of them fill the first
	// two rows; the third stays empty rather than the tiles growing to fill it.
	lv_obj_t *grid = gridpage_build(streaming_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	gridpage_reserve_bottom(grid, navbar_use_shared_mini(streaming_screen, cfg));

	// The design's tiles: taller, a line under the name saying what each
	// needs, and the two services this build cannot reach drawn quietly, with
	// a plain glyph instead of their coloured marks.
	bool tidal_ok = tidal_configured(), qobuz_ok = qobuz_configured();
	const struct {
		const char *line;
		const lv_image_dsc_t *glyph;
		bool off;
	} TILES[] = {
		{"streaming_needs_wifi", NULL, false},
		{"streaming_on_card", NULL, false},
		{tidal_ok ? "streaming_needs_wifi" : "streaming_not_in_build", &icon_audio_waveform, !tidal_ok},
		{qobuz_ok ? "streaming_needs_wifi" : "streaming_not_in_build", &icon_album, !qobuz_ok},
	};
	for (uint32_t i = 0; i < lv_obj_get_child_count(grid) && i < sizeof(TILES) / sizeof(TILES[0]); i++) {
		lv_obj_t *tile = lv_obj_get_child(grid, (int32_t)i);
		lv_obj_t *icon = lv_obj_get_child(tile, 0);
		lv_obj_t *name = lv_obj_get_child(tile, 1);
		if (!icon || !name) {
			continue;
		}
		lv_obj_set_height(tile, 132);
		lv_obj_set_style_pad_hor(tile, 18, 0);
		lv_obj_set_style_pad_ver(tile, 18, 0);
		if (TILES[i].glyph) {
			lv_image_set_src(icon, TILES[i].glyph);
			lv_obj_add_style(icon, &theme_style_nav_icon, 0);
		}
		lv_obj_set_size(icon, 36, 36);
		if (TILES[i].off) {
			lv_obj_set_style_image_recolor(icon, theme()->text_secondary, 0);
			lv_obj_set_style_image_opa(icon, LV_OPA_60, 0);
			lv_obj_add_style(name, &theme_style_text_dim, 0);
		}
		lv_obj_t *text = lv_obj_create(tile);
		lv_obj_remove_style_all(text);
		lv_obj_set_size(text, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(text, 2, 0);
		lv_obj_set_event_bubble(text, true);
		lv_obj_set_parent(name, text);
		lv_obj_t *line = lv_label_create(text);
		lv_label_set_text(line, tr(TILES[i].line));
		lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
		lv_obj_set_width(line, lv_pct(100));
		lv_obj_add_style(line, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(line, &font_ui_18, 0);
	}
	if (!tidal_ok || !qobuz_ok) {
		lv_obj_t *note = lv_label_create(grid);
		lv_label_set_text(note, tr("streaming_keys_note"));
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_18, 0);
		lv_obj_set_style_pad_hor(note, 12, 0);
		lv_obj_set_style_pad_top(note, 10, 0);
	}

	settingsrow_title(streaming_screen, cfg, "streaming");
}
