#include "main_menu.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "src/gui/audio/dacpage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/system/device/clock.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/filespage.h"
#include "src/gui/library/music.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"

lv_obj_t *main_menu_screen;
lv_obj_t *main_menu_apps_screen;

#define HOME_COVER_MAX 256
// One RGB565 copy of the cover at the size it is shown (at most 128 KiB), with
// no decoder, file I/O or full-resolution intermediate on the GUI thread. Never borrow cover pixels
// across a track change: the player owns and frees those independently.
static LV_ATTRIBUTE_MEM_ALIGN uint16_t cover_pixels[HOME_COVER_MAX * HOME_COVER_MAX];
static unsigned cover_size = HOME_COVER_MAX;
static lv_image_dsc_t home_cover = {
	.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
		.w = HOME_COVER_MAX, .h = HOME_COVER_MAX, .stride = HOME_COVER_MAX * 2},
	.data_size = sizeof(cover_pixels), .data = (const uint8_t *)cover_pixels,
};
static uint64_t cover_identity;
static lv_obj_t *artwork;
static lv_obj_t *artwork_note;
static lv_obj_t *track_title;
static lv_obj_t *track_artist;
static lv_obj_t *elapsed_label;
static lv_obj_t *duration_label;
static lv_obj_t *progress_bar;
static lv_obj_t *transport_icon;
static lv_obj_t *transport_buttons[3];
static lv_obj_t *queue_label;
static lv_obj_t *mode_icon;
static lv_obj_t *star_icon;
static bool seeking;
static lv_obj_t *weekday_label;
static lv_obj_t *date_label;
static int date_shown = -1;
static void refresh_date(void);

typedef struct {
	const char *label;
	const lv_image_dsc_t *icon;
	lv_obj_t **screen;
	void (*open)(void);
	uint32_t top, bottom; // icon gradient, lighter edge first
} home_app_t;

static const home_app_t apps[] = {
	{"music", &icon_menu_music, &music_screen, NULL, 0xFF5E6E, 0xFA2D48},
	{"books", &icon_menu_books, NULL, ebookpage_open, 0xFFB340, 0xFF8A00},
	{"audiobooks", &icon_menu_audiobooks, &audiobooks_screen, NULL, 0x7D7AFF, 0x5451D6},
	{"streaming", &icon_menu_streaming, &streaming_screen, NULL, 0xD77BFF, 0xAF52DE},
	{"wireless", &icon_menu_wireless, &wireless_screen, NULL, 0x4DA3FF, 0x0A6CFF},
	{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open, 0x5AC8F5, 0x1E96D8},
	{"dac", &icon_menu_dac, &dacpage_screen, NULL, 0x4CD964, 0x28B446},
	{"more", &icon_menu_more, &morepage_screen, NULL, 0xAEAEB2, 0x7C7C80},
	{"settings", &icon_menu_settings, &settings_screen, NULL, 0x8E8E93, 0x5B5B60},
};
static const home_app_t drawer_app = {"home_apps", &icon_apps, &main_menu_apps_screen, NULL, 0x5A5A5E, 0x2C2C2E};

static bool gesture_active(void) {
	return player_sheet_drag_active() || switcher_back_drag_active();
}

static void open_app_cb(lv_event_t *event) {
	if (gesture_active()) return;
	const home_app_t *app = lv_event_get_user_data(event);
	if (app->open) app->open();
	else if (app->screen && *app->screen) switch_screen(*app->screen);
}

static void open_player_cb(lv_event_t *event) {
	(void)event;
	if (!gesture_active()) player_sheet_open(true);
}

static void update_artwork(void) {
	uint64_t identity = player_cover_identity();
	const lv_image_dsc_t *source = identity ? player_cover_image() : NULL;
	bool valid = source && source->data && source->header.cf == LV_COLOR_FORMAT_RGB565 &&
		source->header.w && source->header.h && source->header.stride >= source->header.w * 2 &&
		source->data_size >= (uint32_t)source->header.stride * source->header.h;
	lv_obj_set_hidden(artwork, !valid);
	lv_obj_set_hidden(artwork_note, valid);
	if (!valid) {
		cover_identity = 0;
		return;
	}
	if (identity == cover_identity) return;
	for (unsigned y = 0; y < cover_size; y++) {
		unsigned sy = y * source->header.h / cover_size;
		const uint8_t *row = source->data + sy * source->header.stride;
		for (unsigned x = 0; x < cover_size; x++) {
			unsigned sx = x * source->header.w / cover_size;
			memcpy(&cover_pixels[y * cover_size + x], row + sx * 2, 2);
		}
	}
	cover_identity = identity;
	lv_image_cache_drop(&home_cover);
	lv_obj_invalidate(artwork);
}

static void set_text_if_changed(lv_obj_t *label, const char *text) {
	if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void time_text(lv_obj_t *label, double seconds) {
	unsigned value = seconds > 0 && seconds < 360000 ? (unsigned)seconds : 0;
	char text[24];
	if (value >= 3600) snprintf(text, sizeof(text), "%u:%02u:%02u", value / 3600, value / 60 % 60, value % 60);
	else snprintf(text, sizeof(text), "%u:%02u", value / 60, value % 60);
	set_text_if_changed(label, text);
}

// Queue position, playback mode and the star. All three are read from state
// the player already keeps; nothing is queried from the card or the network.
static void refresh_extras(bool track) {
	int position = 0, count = 0;
	char text[32] = "";
	if (track && player_queue_position(&position, &count)) snprintf(text, sizeof(text), "%d / %d", position, count);
	set_text_if_changed(queue_label, text);

	const lv_image_dsc_t *glyph = &icon_repeat_off;
	bool active = true;
	switch (playlist_get_mode()) {
	case PLAYBACK_MODE_REPEAT_ONE: glyph = &icon_repeat_one; break;
	case PLAYBACK_MODE_REPEAT_ALL: glyph = &icon_repeat_all; break;
	case PLAYBACK_MODE_SHUFFLE: glyph = &icon_shuffle; break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT: glyph = &icon_shuffle_repeat; break;
	default: active = false; break;
	}
	if (lv_image_get_src(mode_icon) != glyph) lv_image_set_src(mode_icon, glyph);
	lv_obj_set_style_image_recolor(mode_icon, active ? theme()->accent : theme()->text_secondary, 0);

	bool starred = track && player_favourite_starred();
	const lv_image_dsc_t *star = starred ? &icon_star_filled : &icon_star;
	if (lv_image_get_src(star_icon) != star) lv_image_set_src(star_icon, star);
	lv_obj_set_style_image_recolor(star_icon, starred ? lv_color_hex(0xFFCC00) : theme()->text_secondary, 0);
	lv_obj_set_state(lv_obj_get_parent(star_icon), LV_STATE_DISABLED, !track);
}

// Runs from the existing playback update and on entry. No additional timer
// is created, and hidden home screens perform no artwork or label work.
void main_menu_refresh_player(const device_state_t *state) {
	if (!track_title || lv_screen_active() != main_menu_screen || player_sheet_is_open()) return;
	refresh_date();
	bool loaded = state->live || state->current_file[0];
	const char *file = strrchr(state->current_file, '/');
	const char *title = state->metadata.title;
	if (!title[0]) title = state->current_file[0] ? (file ? file + 1 : state->current_file) : tr("player_no_track_loaded");
	set_text_if_changed(track_title, title);
	set_text_if_changed(track_artist, loaded ? state->metadata.artist : "");
	const lv_image_dsc_t *glyph = state->status == AUDIO_STATUS_PLAYING ? &icon_pause_large : &icon_play_large;
	if (lv_image_get_src(transport_icon) != glyph) lv_image_set_src(transport_icon, glyph);
	for (int i = 0; i < 3; i++) lv_obj_set_state(transport_buttons[i], LV_STATE_DISABLED, !loaded);
	int progress = !state->live && state->progress_total_secs > 0 ?
		(int)(1000.0 * state->progress_current_secs / state->progress_total_secs) : 0;
	if (progress < 0) progress = 0;
	if (progress > 1000) progress = 1000;
	if (!seeking && lv_slider_get_value(progress_bar) != progress) lv_slider_set_value(progress_bar, progress, LV_ANIM_OFF);
	refresh_extras(loaded && !state->live);
	time_text(elapsed_label, state->live ? 0 : state->progress_current_secs);
	time_text(duration_label, state->live ? 0 : state->progress_total_secs);
	lv_obj_set_hidden(progress_bar, state->live);
	lv_obj_set_hidden(elapsed_label, state->live);
	lv_obj_set_hidden(duration_label, state->live);
	update_artwork();
}

static void refresh_snapshot(void) {
	device_state_t state;
	device_state_get(&state);
	main_menu_refresh_player(&state);
}

static void transport_cb(lv_event_t *event) {
	if (gesture_active()) return;
	int action = (int)(intptr_t)lv_event_get_user_data(event);
	if (action < 0) player_key_prev();
	else if (action > 0) player_key_next();
	else player_key_play_pause();
	refresh_snapshot();
}

static void screen_loaded_cb(lv_event_t *event) {
	(void)event;
	refresh_date();
	refresh_snapshot();
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, bool secondary) {
	lv_obj_t *text = lv_label_create(parent);
	lv_label_set_text_static(text, "");
	lv_obj_add_style(text, secondary ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(text, font, 0);
	return text;
}

static void extra_cb(lv_event_t *event) {
	if (gesture_active()) return;
	if (lv_event_get_user_data(event)) player_favourite_toggle();
	else player_mode_cycle();
	refresh_snapshot();
}

// Drag or tap the progress line to seek. The snapshot leaves the line alone
// while a finger is on it; one seek is issued when the finger lifts.
static void seek_cb(lv_event_t *event) {
	lv_event_code_t code = lv_event_get_code(event);
	if (code == LV_EVENT_PRESSED) {
		seeking = true;
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (seeking && code == LV_EVENT_RELEASED) {
			device_state_t state;
			device_state_get(&state);
			if (!state.live && state.progress_total_secs > 0)
				device_state_seek(state.progress_total_secs * lv_slider_get_value(progress_bar) / 1000.0);
		}
		seeking = false;
		refresh_snapshot();
	}
}

static void refit_home_text(void) {
	if (!track_title) return;
	lv_obj_set_height(track_title, lv_font_get_line_height(&font_ui_24_bold));
	lv_obj_set_height(track_artist, lv_font_get_line_height(&font_ui_20));
}

// One large Now Playing widget: the artwork, the title and artist beneath it,
// a thin progress line and the three transport glyphs. Its height follows its
// content, so there is no empty band whatever the screen height.
static void make_player(lv_obj_t *screen, gui_config_t *cfg, int top, int height) {
	const int pad = 22, below = 206; // everything under the artwork
	int width = cfg->screen_width - 2 * cfg->padding;
	int art = height - pad - below;
	if (art > HOME_COVER_MAX) art = HOME_COVER_MAX;
	if (art < 96) art = 96;
	cover_size = (unsigned)art;
	home_cover.header.w = home_cover.header.h = (uint32_t)art;
	home_cover.header.stride = (uint32_t)art * 2;
	home_cover.data_size = (uint32_t)art * art * 2;
	int spare = height - (pad + art + below);
	height = pad + art + below;
	if (spare > 0) top += spare / 2; // sit midway between the date and the dock

	lv_obj_t *card = lv_obj_create(screen);
	lv_obj_remove_style_all(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_size(card, width, height);
	lv_obj_align(card, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_radius(card, 32, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_add_event_cb(card, open_player_cb, LV_EVENT_CLICKED, NULL);
	player_sheet_attach_drag(card, true);

	lv_obj_t *cover = lv_obj_create(card);
	lv_obj_remove_style_all(cover);
	lv_obj_add_style(cover, &theme_style_screen, 0);
	lv_obj_set_size(cover, art, art);
	lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, pad);
	lv_obj_set_style_radius(cover, 20, 0);
	lv_obj_set_style_clip_corner(cover, true, 0);
	lv_obj_set_clickable(cover, false);
	lv_obj_set_scrollable(cover, false);
	artwork = lv_image_create(cover);
	lv_image_set_src(artwork, &home_cover);
	artwork_note = lv_image_create(cover);
	lv_image_set_src(artwork_note, &icon_music_note);
	lv_obj_set_size(artwork_note, 72, 72);
	lv_image_set_inner_align(artwork_note, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_add_style(artwork_note, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(artwork_note, LV_OPA_30, 0);
	lv_obj_center(artwork_note);
	lv_obj_set_hidden(artwork, true);

	int y = pad + art + 16;
	track_title = label(card, &font_ui_24_bold, false);
	lv_obj_set_width(track_title, width - 2 * pad);
	lv_label_set_long_mode(track_title, LV_LABEL_LONG_DOT);
	lv_obj_set_pos(track_title, pad, y);
	track_artist = label(card, &font_ui_20, true);
	lv_obj_set_width(track_artist, width - 2 * pad);
	lv_label_set_long_mode(track_artist, LV_LABEL_LONG_DOT);
	lv_obj_set_pos(track_artist, pad, y + 34);
	refit_home_text();
	fonts_register_change(refit_home_text);

	progress_bar = lv_slider_create(card);
	lv_obj_remove_style_all(progress_bar);
	lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_ext_click_area(progress_bar, 16);
	lv_obj_add_event_cb(progress_bar, seek_cb, LV_EVENT_ALL, NULL);
	lv_obj_set_size(progress_bar, width - 2 * pad, 6);
	lv_obj_set_pos(progress_bar, pad, y + 76);
	lv_obj_set_style_radius(progress_bar, 3, LV_PART_MAIN);
	lv_obj_set_style_radius(progress_bar, 3, LV_PART_INDICATOR);
	lv_slider_set_range(progress_bar, 0, 1000);
	lv_obj_add_style(progress_bar, &theme_style_slider_track, LV_PART_MAIN);
	lv_obj_set_style_bg_color(progress_bar, theme()->text_primary, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
	elapsed_label = label(card, &font_ui_16, true);
	duration_label = label(card, &font_ui_16, true);
	lv_obj_set_pos(elapsed_label, pad, y + 88);
	lv_obj_align(duration_label, LV_ALIGN_TOP_RIGHT, -pad, y + 88);
	queue_label = label(card, &font_ui_16, true);
	lv_obj_align(queue_label, LV_ALIGN_TOP_MID, 0, y + 88);
	for (int action = -1; action <= 1; action++) {
		lv_obj_t *button = lv_btn_create(card);
		lv_obj_remove_style_all(button);
		lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_size(button, 80, 64);
		lv_obj_set_style_radius(button, 20, 0);
		lv_obj_set_style_opa(button, LV_OPA_30, LV_STATE_DISABLED);
		lv_obj_align(button, LV_ALIGN_BOTTOM_MID, action * 92, -12);
		lv_obj_add_event_cb(button, transport_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);
		player_sheet_attach_drag(button, true);
		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, action < 0 ? &icon_skip_back_large : action > 0 ? &icon_skip_forward_large : &icon_play_large);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
		transport_buttons[action + 1] = button;
		if (!action) transport_icon = icon;
	}
	// Playback mode on the left of the transport, the star on the right.
	for (int side = 0; side < 2; side++) {
		lv_obj_t *button = lv_btn_create(card);
		lv_obj_remove_style_all(button);
		lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_size(button, 56, 56);
		lv_obj_set_style_radius(button, 18, 0);
		lv_obj_set_style_opa(button, LV_OPA_30, LV_STATE_DISABLED);
		lv_obj_align(button, side ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, side ? -pad + 10 : pad - 10, -16);
		lv_obj_add_event_cb(button, extra_cb, LV_EVENT_CLICKED, (void *)(intptr_t)side);
		player_sheet_attach_drag(button, true);
		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, side ? &icon_star : &icon_repeat_off);
		lv_obj_set_size(icon, 28, 28);
		lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		lv_obj_center(icon);
		if (side) star_icon = icon;
		else mode_icon = icon;
	}
}

// The header is today's date, as on a lock screen. It is recomputed only when
// the day changes, on entry and from the existing playback snapshot.
static void refresh_date(void) {
	if (!date_label) return;
	char weekday[32] = "", date[48] = "";
	int day = 0;
	if (clock_is_set()) {
		time_t now = time(NULL);
		struct tm local;
		localtime_r(&now, &local);
		day = local.tm_yday + 1;
		if (day == date_shown) return;
		strftime(weekday, sizeof(weekday), "%A", &local);
		for (char *c = weekday; *c; c++) *c = (char)toupper((unsigned char)*c);
		strftime(date, sizeof(date), "%e %B", &local);
	} else if (date_shown == 0) {
		return;
	}
	date_shown = day;
	lv_label_set_text(weekday_label, weekday);
	lv_label_set_text(date_label, date[0] == ' ' ? date + 1 : date);
}

// Application icons: a soft two-stop gradient under a white glyph. The fill
// is drawn directly by the software renderer; no image or layer is allocated.
static void make_app(lv_obj_t *parent, const home_app_t *app, int width, bool dock) {
	const int size = dock ? 68 : 76;
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_set_size(button, width, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(button, dock ? size : 132, 0);
	lv_obj_set_scrollable(button, false);
	lv_obj_set_event_bubble(button, true);
	lv_obj_add_event_cb(button, open_app_cb, LV_EVENT_CLICKED, (void *)app);

	lv_obj_t *square = lv_obj_create(button);
	lv_obj_remove_style_all(square);
	lv_obj_set_size(square, size, size);
	lv_obj_set_style_radius(square, size * 23 / 100, 0);
	lv_obj_set_style_bg_opa(square, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(square, lv_color_hex(app->top), 0);
	lv_obj_set_style_bg_grad_color(square, lv_color_hex(app->bottom), 0);
	lv_obj_set_style_bg_grad_dir(square, LV_GRAD_DIR_VER, 0);
	lv_obj_align(square, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_clickable(square, false);
	lv_obj_set_scrollable(square, false);
	// Pressing dims the icon itself, as a touch on a home-screen icon does.
	lv_obj_set_style_opa(button, LV_OPA_60, LV_STATE_PRESSED);
	lv_obj_t *icon = lv_image_create(square);
	lv_image_set_src(icon, app->icon);
	lv_obj_set_size(icon, size / 2, size / 2);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	lv_obj_center(icon);
	if (dock) return;
	lv_obj_t *caption = label(button, &font_ui_18, false);
	lv_label_set_text(caption, tr(app->label));
	lv_obj_set_width(caption, width - 4);
	lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, size + 10);
}

// A brief vertical settle uses ordinary object positions: no screen capture,
// blur or opacity layer. Cancel it on exit so quick navigation leaves no work.
static void drawer_translate(void *grid, int32_t value) {
	lv_obj_set_style_translate_y(grid, value, 0);
}

static void drawer_motion_cb(lv_event_t *event) {
	lv_obj_t *grid = lv_event_get_user_data(event);
	lv_anim_delete(grid, drawer_translate);
	if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOADED) {
		drawer_translate(grid, 0);
		return;
	}
	lv_anim_t animation;
	lv_anim_init(&animation);
	lv_anim_set_var(&animation, grid);
	lv_anim_set_values(&animation, 12, 0);
	lv_anim_set_duration(&animation, 160);
	lv_anim_set_exec_cb(&animation, drawer_translate);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_start(&animation);
}

static void make_drawer(gui_config_t *cfg) {
	main_menu_apps_screen = lv_obj_create(NULL);
	lv_obj_add_style(main_menu_apps_screen, &theme_style_screen, 0);
	settingsrow_title(main_menu_apps_screen, cfg, "home_apps");
	lv_obj_t *grid = lv_obj_create(main_menu_apps_screen);
	lv_obj_remove_style_all(grid);
	int top = settingsrow_content_top(cfg);
	lv_obj_set_size(grid, cfg->screen_width, cfg->screen_height - top);
	lv_obj_set_pos(grid, 0, top);
	lv_obj_set_style_pad_all(grid, cfg->padding, 0);
	lv_obj_set_style_pad_gap(grid, 12, 0);
	lv_obj_set_style_pad_row(grid, 18, 0);
	lv_obj_set_style_pad_top(grid, 20, 0);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_scroll_elastic(grid, false);
	for (unsigned i = 0; i < sizeof(apps) / sizeof(apps[0]); i++)
		make_app(grid, &apps[i], (cfg->screen_width - 2 * cfg->padding - 24) / 3, false);
	player_sheet_attach_drag(grid, true);
	switcher_attach_back_gesture(grid);
	lv_obj_add_event_cb(main_menu_apps_screen, drawer_motion_cb, LV_EVENT_SCREEN_LOADED, grid);
	lv_obj_add_event_cb(main_menu_apps_screen, drawer_motion_cb, LV_EVENT_SCREEN_UNLOADED, grid);
}

void main_menu_init(gui_config_t *cfg) {
	lv_obj_add_style(main_menu_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(main_menu_screen, false);
	weekday_label = label(main_menu_screen, &font_ui_18, false);
	lv_obj_set_style_text_color(weekday_label, theme()->accent, 0);
	lv_obj_set_style_text_letter_space(weekday_label, 1, 0);
	lv_obj_align(weekday_label, LV_ALIGN_TOP_LEFT, cfg->padding + 10, cfg->top_bar_height + 14);
	date_label = label(main_menu_screen, &font_ui_36_bold, false);
	lv_obj_align(date_label, LV_ALIGN_TOP_LEFT, cfg->padding + 10, cfg->top_bar_height + 36);

	// Date, one widget, the dock.
	const int dock_height = 100;
	int card_top = cfg->top_bar_height + 96;
	int dock_top = cfg->screen_height - cfg->padding - dock_height;
	make_player(main_menu_screen, cfg, card_top, dock_top - 16 - card_top);
	make_drawer(cfg);
	// The dock is one quiet bar holding the four everyday destinations.
	lv_obj_t *dock = lv_obj_create(main_menu_screen);
	lv_obj_remove_style_all(dock);
	lv_obj_add_style(dock, &theme_style_card, 0);
	lv_obj_set_size(dock, cfg->screen_width - 2 * cfg->padding, 100);
	lv_obj_set_style_radius(dock, 32, 0);
	lv_obj_set_style_pad_hor(dock, 18, 0);
	lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -cfg->padding);
	lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(dock, false);
	int width = (cfg->screen_width - 2 * cfg->padding - 36) / 4;
	make_app(dock, &apps[0], width, true);
	make_app(dock, &apps[1], width, true);
	make_app(dock, &apps[8], width, true);
	make_app(dock, &drawer_app, width, true);
	player_sheet_attach_drag(dock, true);
	lv_obj_add_event_cb(main_menu_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
