#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/filespage.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/navbar.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/airplay.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/system/core/lang.h"

lv_obj_t *morepage_screen;

// Everything that is not music, books or search, in three groups. Each group
// starts with a full-width heading, which breaks the two-column flow of tiles.
typedef struct {
	const char *heading; // NULL for a tile
	grid_entry_t entry;
} more_item_t;

static void add_heading(lv_obj_t *grid, const char *text, int index) {
	lv_obj_t *label = lv_label_create(grid);
	lv_label_set_text(label, tr(text));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_16_bold, 0);
	lv_obj_set_style_text_letter_space(label, 1, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_pad_top(label, index ? 14 : 4, 0);
	lv_obj_set_style_pad_bottom(label, 2, 0);
	lv_obj_set_style_pad_left(label, 8, 0);
	lv_obj_move_to_index(label, index);
}

void morepage_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		// Connect
		{"wi_fi", &icon_menu_wifi_settings, &wifisettings_screen, NULL},
		{"bluetooth", &icon_menu_bluetooth, &btsettings_screen, NULL},
		{"airplay", &icon_menu_airplay, &airplay_screen, NULL},
		{"transfer", &icon_menu_wifi_transfer, &wifitransfer_screen, NULL},
		{"sonixlink", &icon_menu_sonixlink, &sonixlink_screen, NULL},
		{"dlna", &icon_menu_dlna, &dlna_screen, NULL},
		// Sources
		{"streaming", &icon_menu_streaming, &streaming_screen, NULL},
		// An action: the listing is read when the page is entered, so a card
		// that changed under the player shows what is on it now.
		{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
		{"dac", &icon_menu_dac, &dacpage_screen, NULL},
		// Tools
		{"metronome", &icon_menu_metronome, &metronomepage_screen, NULL},
#if GB_CORE
		// The Game Boy emulator, present only when built with GB=1.
		{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
		// An action: the artwork is read from the resource tree on the way in.
		{"flappy_bird", &icon_menu_flappy_glyph, NULL, flappybird_open},
	};
	const int count = (int)(sizeof(entries) / sizeof(entries[0]));

	lv_obj_t *grid = gridpage_build(morepage_screen, cfg, entries, count, 3, 0, true);

	// Headings go in last-first, so the indices of the earlier ones still hold.
	add_heading(grid, "more_tools", 9);
	add_heading(grid, "more_sources", 6);
	add_heading(grid, "more_connect", 0);

	gridpage_reserve_bottom(grid, navbar_attach(morepage_screen, cfg, NAVBAR_MORE, true));

	lv_obj_t *heading = settingsrow_title(morepage_screen, cfg, "more");
	settingsrow_title_root(heading, cfg);

	// Settings is a chip beside the heading rather than a tile among the apps.
	lv_obj_t *chip = lv_btn_create(morepage_screen);
	lv_obj_remove_style_all(chip);
	lv_obj_add_style(chip, &theme_style_card_pressed, 0);
	lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(chip, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_set_size(chip, LV_SIZE_CONTENT, 52);
	lv_obj_set_style_radius(chip, 26, 0);
	lv_obj_set_style_pad_hor(chip, 20, 0);
	lv_obj_set_style_pad_column(chip, 8, 0);
	lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -12, cfg->top_bar_height + cfg->padding + 2);
	lv_obj_add_event_cb(chip, switch_screen_cb, LV_EVENT_CLICKED, settings_screen);
	lv_obj_t *gear = lv_image_create(chip);
	lv_image_set_src(gear, &icon_menu_settings);
	lv_obj_set_size(gear, 24, 24);
	lv_image_set_inner_align(gear, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_add_style(gear, &theme_style_icon, 0);
	lv_obj_t *chip_text = lv_label_create(chip);
	lv_label_set_text(chip_text, tr("settings"));
	lv_obj_add_style(chip_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(chip_text, &font_ui_18_bold, 0);
}
