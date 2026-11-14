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
#include "src/gui/nowplaying/coverloader.h"
#include <stdlib.h>
#include "src/gui/settings/settings.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/navbar.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/system/library/library.h"
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
static lv_obj_t *queue_label; // the format badge under the artist
static lv_obj_t *state_label;
static lv_obj_t *home_text_col;
static lv_obj_t *home_progress;
static int home_text_top, home_text_art;
static void recent_rebuild(void);
static void recent_poll(void);
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

static bool set_text_if_changed(lv_obj_t *label, const char *text) {
	if (strcmp(lv_label_get_text(label), text) == 0) {
		return false;
	}
	lv_label_set_text(label, text);
	return true;
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
static bool refresh_extras(bool track) {
	// The badge under the artist: the format, as the player words it.
	const char *format = track ? player_format_text() : "";
	bool changed = set_text_if_changed(queue_label, format);
	lv_obj_set_hidden(queue_label, !format[0]);

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
	return changed;
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
	bool relayout = set_text_if_changed(track_title, title);
	relayout |= set_text_if_changed(track_artist, loaded ? state->metadata.artist : "");
	relayout |= set_text_if_changed(state_label, !loaded ? "" : tr(state->status == AUDIO_STATUS_PLAYING ? "home_playing" : "home_paused"));
	const lv_image_dsc_t *glyph = state->status == AUDIO_STATUS_PLAYING ? &icon_pause_large : &icon_play_large;
	if (lv_image_get_src(transport_icon) != glyph) lv_image_set_src(transport_icon, glyph);
	for (int i = 0; i < 3; i++) lv_obj_set_state(transport_buttons[i], LV_STATE_DISABLED, !loaded);
	int progress = !state->live && state->progress_total_secs > 0 ?
		(int)(1000.0 * state->progress_current_secs / state->progress_total_secs) : 0;
	if (progress < 0) progress = 0;
	if (progress > 1000) progress = 1000;
	if (!seeking && lv_slider_get_value(progress_bar) != progress) lv_slider_set_value(progress_bar, progress, LV_ANIM_OFF);
	relayout |= refresh_extras(loaded && !state->live);
	time_text(elapsed_label, state->live ? 0 : state->progress_current_secs);
	time_text(duration_label, state->live ? 0 : state->progress_total_secs);
	lv_obj_set_hidden(progress_bar, state->live);
	lv_obj_set_hidden(elapsed_label, state->live);
	lv_obj_set_hidden(duration_label, state->live);
	update_artwork();
	if (home_text_col && relayout) {
		lv_obj_update_layout(home_text_col);
		int ty = home_text_top + (home_text_art - lv_obj_get_height(home_text_col)) / 2;
		if (lv_obj_get_y(home_text_col) != ty) lv_obj_set_y(home_text_col, ty);
	}
	recent_poll();
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
	recent_rebuild();
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
	lv_obj_set_height(track_title, lv_font_get_line_height(&font_ui_28_bold));
	lv_obj_set_height(track_artist, lv_font_get_line_height(&font_ui_20));
}

static lv_obj_t *round_button(lv_obj_t *parent, int size, lv_event_cb_t cb, intptr_t user) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_size(button, size, size);
	lv_obj_set_style_radius(button, 18, 0);
	lv_obj_set_style_opa(button, LV_OPA_30, LV_STATE_DISABLED);
	if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)user);
	player_sheet_attach_drag(button, true);
	return button;
}

static lv_obj_t *glyph(lv_obj_t *button, const lv_image_dsc_t *src, int size) {
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, src);
	lv_obj_set_size(icon, size, size);
	lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return icon;
}

// The Now Playing card, as the design draws it: the artwork with a state label,
// the title, the artist and the format badge beside it; elapsed, a thin line
// and the length on one row; then the transport -- mode, previous, an 88 px
// play/pause, next and the star, every one a 72 px target or more.
// Returns the card's height.
static int make_player(lv_obj_t *screen, gui_config_t *cfg, int top, int art) {
	const int pad = 16;
	int width = cfg->screen_width - 2 * 12;
	cover_size = (unsigned)art;
	home_cover.header.w = home_cover.header.h = (uint32_t)art;
	home_cover.header.stride = (uint32_t)art * 2;
	home_cover.data_size = (uint32_t)art * art * 2;
	const int progress_h = 24, transport_h = 88, gap = 14;
	int height = pad + art + gap + progress_h + gap + transport_h + 12;

	lv_obj_t *card = lv_obj_create(screen);
	lv_obj_remove_style_all(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_size(card, width, height);
	lv_obj_align(card, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_radius(card, 20, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, true);
	lv_obj_add_event_cb(card, open_player_cb, LV_EVENT_CLICKED, NULL);
	player_sheet_attach_drag(card, true);

	lv_obj_t *cover = lv_obj_create(card);
	lv_obj_remove_style_all(cover);
	lv_obj_add_style(cover, &theme_style_card_pressed, 0);
	lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
	lv_obj_set_size(cover, art, art);
	lv_obj_set_pos(cover, pad, pad);
	lv_obj_set_style_radius(cover, 10, 0);
	lv_obj_set_clickable(cover, false);
	lv_obj_set_scrollable(cover, false);
	// Rounded by the image itself (LVGL clips an image to its own radius as it
	// draws), not by clip_corner on the frame: on an object with children that
	// renders the frame into an offscreen layer on every redraw of it.
	artwork = lv_image_create(cover);
	lv_obj_set_style_radius(artwork, 10, 0);
	lv_image_set_src(artwork, &home_cover);
	artwork_note = lv_image_create(cover);
	lv_image_set_src(artwork_note, &icon_music_note);
	lv_obj_set_size(artwork_note, art / 2, art / 2);
	lv_image_set_inner_align(artwork_note, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_add_style(artwork_note, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(artwork_note, LV_OPA_30, 0);
	lv_obj_center(artwork_note);
	lv_obj_set_hidden(artwork, true);

	// State, title, artist and format, vertically centred on the artwork.
	int text_x = pad + art + 16, text_w = width - text_x - pad;
	lv_obj_t *text = lv_obj_create(card);
	lv_obj_remove_style_all(text);
	lv_obj_set_size(text, text_w, LV_SIZE_CONTENT);
	lv_obj_set_pos(text, text_x, 0);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_gap(text, 3, 0);
	lv_obj_set_clickable(text, false);
	lv_obj_set_scrollable(text, false);
	state_label = label(text, &font_ui_16_bold, true);
	lv_obj_set_style_text_letter_space(state_label, 1, 0);
	lv_obj_set_style_text_opa(state_label, LV_OPA_80, 0);
	track_title = label(text, &font_ui_28_bold, false);
	lv_obj_set_width(track_title, text_w);
	lv_label_set_long_mode(track_title, LV_LABEL_LONG_DOT);
	track_artist = label(text, &font_ui_20, true);
	lv_obj_set_width(track_artist, text_w);
	lv_label_set_long_mode(track_artist, LV_LABEL_LONG_DOT);
	queue_label = label(text, &font_ui_16_bold, true);
	lv_obj_set_style_border_width(queue_label, 2, 0);
	lv_obj_set_style_border_color(queue_label, theme()->surface_pressed, 0);
	lv_obj_set_style_radius(queue_label, 6, 0);
	lv_obj_set_style_pad_hor(queue_label, 8, 0);
	lv_obj_set_style_pad_ver(queue_label, 2, 0);
	lv_obj_set_style_margin_top(queue_label, 6, 0);
	lv_obj_set_style_max_width(queue_label, text_w, 0);
	lv_label_set_long_mode(queue_label, LV_LABEL_LONG_DOT);
	refit_home_text();
	fonts_register_change(refit_home_text);
	lv_obj_update_layout(text);
	lv_obj_set_y(text, pad + (art - lv_obj_get_height(text)) / 2);
	home_text_col = text;
	home_text_top = pad;
	home_text_art = art;

	// Progress: elapsed, the line, the length -- one row, so the numbers never
	// sit under a finger dragging the line.
	int y = pad + art + gap;
	elapsed_label = label(card, &font_ui_18, true);
	lv_obj_set_width(elapsed_label, 60);
	lv_obj_align(elapsed_label, LV_ALIGN_TOP_LEFT, pad, y);
	duration_label = label(card, &font_ui_18, true);
	lv_obj_set_width(duration_label, 60);
	lv_obj_set_style_text_align(duration_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_align(duration_label, LV_ALIGN_TOP_RIGHT, -pad, y);
	progress_bar = lv_slider_create(card);
	lv_obj_remove_style_all(progress_bar);
	lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_ext_click_area(progress_bar, 20);
	lv_obj_add_event_cb(progress_bar, seek_cb, LV_EVENT_ALL, NULL);
	lv_obj_set_size(progress_bar, width - 2 * pad - 2 * 66, 6);
	lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, y + 9);
	lv_obj_set_style_radius(progress_bar, 3, LV_PART_MAIN);
	lv_obj_set_style_radius(progress_bar, 3, LV_PART_INDICATOR);
	lv_slider_set_range(progress_bar, 0, 1000);
	lv_obj_set_style_bg_color(progress_bar, theme()->surface_pressed, LV_PART_MAIN);
	lv_obj_add_style(progress_bar, &theme_style_accent_bg, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
	home_progress = progress_bar;

	// Transport.
	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, width - 2 * pad, transport_h);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y + progress_h + gap);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(row, false);
	lv_obj_set_scrollable(row, false);

	lv_obj_t *mode = round_button(row, 72, extra_cb, 0);
	mode_icon = glyph(mode, &icon_repeat_off, 30);
	lv_obj_remove_style(mode_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(mode_icon, LV_OPA_COVER, 0);
	for (int action = -1; action <= 1; action++) {
		lv_obj_t *button = round_button(row, action ? 72 : 88, transport_cb, action);
		lv_obj_t *icon;
		if (action) {
			icon = glyph(button, action < 0 ? &icon_skip_back_large : &icon_skip_forward_large, 44);
		} else {
			lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
			lv_obj_add_style(button, &theme_style_accent_bg, 0);
			lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
			icon = glyph(button, &icon_play_large, 44);
			lv_obj_remove_style(icon, &theme_style_icon, 0);
			lv_obj_add_style(icon, &theme_style_accent_bg, 0);
			lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
			lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
			transport_icon = icon;
		}
		transport_buttons[action + 1] = button;
	}
	lv_obj_t *star = round_button(row, 72, extra_cb, 1);
	star_icon = glyph(star, &icon_star, 30);
	lv_obj_remove_style(star_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(star_icon, LV_OPA_COVER, 0);
	return height;
}

static void open_albums(void) { medialist_open(tr("albums"), LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL); }

// ---------------------------------------------------------------------------
// Jump back in: the three records played most recently, newest first, topped
// up from the head of the album list on a player with no history yet. Covers
// come from the worker, never from this thread, and are asked for once per
// visit; the list is rebuilt only when the history has grown.
// ---------------------------------------------------------------------------

#define RECENT_COUNT 3
#define RECENT_SIZE 144
#define RECENT_SLOT_BASE 108 // coverloader.h: the home screen's three

typedef struct {
	lv_obj_t *tile;
	lv_obj_t *box;
	lv_obj_t *image;
	lv_obj_t *note;
	lv_obj_t *title;
	lv_obj_t *artist;
	cover_image_t cover;
	char value[256]; // the album, as medialist_open() takes it
	char path[512];	 // a track of it, to take the cover from
	bool waiting;
} recent_t;

static recent_t recents[RECENT_COUNT];
static int recent_filled;
static long long recent_revision = -1;
static lv_obj_t *recent_head;

static bool recent_has(const char *value) {
	for (int i = 0; i < recent_filled; i++) {
		if (library_album_same(recents[i].value, value)) return true;
	}
	return false;
}

static void recent_add(const char *value, const char *path) {
	if (recent_filled >= RECENT_COUNT || !value[0] || recent_has(value)) return;
	recent_t *r = &recents[recent_filled++];
	snprintf(r->value, sizeof(r->value), "%s", value);
	snprintf(r->path, sizeof(r->path), "%s", path);
}

static bool recent_album_row(const char *name, const char *path, const char *artist, void *user) {
	(void)artist;
	(void)user;
	char value[256];
	if (path && path[0] && library_track_album_value(path, value, sizeof(value))) recent_add(value, path);
	else if (name && path) recent_add(name, path);
	return recent_filled < RECENT_COUNT;
}

// Collects the three covers as soon as the worker has them, rather than on the
// playback snapshot's next tick (which is slow, and slower while paused). Runs
// only while one is still coming.
static lv_timer_t *recent_timer;

static void recent_timer_cb(lv_timer_t *timer) {
	recent_poll();
	bool waiting = false;
	for (int i = 0; i < recent_filled; i++) {
		waiting = waiting || recents[i].waiting;
	}
	if (!waiting) {
		lv_timer_pause(timer);
	}
}

static void recent_timer_kick(void) {
	if (!recent_timer) {
		recent_timer = lv_timer_create(recent_timer_cb, 60, NULL);
	} else {
		lv_timer_resume(recent_timer);
	}
}

static void recent_paint(void) {
	for (int i = 0; i < RECENT_COUNT; i++) {
		recent_t *r = &recents[i];
		bool used = i < recent_filled;
		lv_obj_set_hidden(r->tile, !used);
		if (!used) continue;
		char title[256] = "", artist[256] = "";
		library_album_title(r->value, title, sizeof(title));
		library_album_artist(r->value, artist, sizeof(artist));
		lv_label_set_text(r->title, title);
		lv_label_set_text(r->artist, artist);
		lv_obj_set_hidden(r->image, true);
		lv_obj_set_hidden(r->note, false);
		lv_image_set_src(r->image, NULL);
		cover_free(&r->cover);
		coverloader_request(RECENT_SLOT_BASE + i, r->path, RECENT_SIZE);
		r->waiting = true;
	}
	recent_timer_kick();
	if (recent_head) lv_obj_set_hidden(recent_head, recent_filled == 0);
}

static void recent_rebuild(void) {
	if (!recents[0].tile) return;
	long long revision = library_history_revision();
	if (revision == recent_revision && recent_filled > 0) return;
	recent_revision = revision;
	recent_filled = 0;

	library_history_item_t *items = NULL;
	int count = library_history_items(&items);
	for (int i = count - 1; i >= 0 && recent_filled < RECENT_COUNT && i >= count - 60; i--) {
		char path[512], name[8], value[256];
		if (library_history_entry(items[i].id, path, sizeof(path), name, sizeof(name)) &&
			library_track_album_value(path, value, sizeof(value))) {
			recent_add(value, path);
		}
	}
	free(items);
	if (recent_filled < RECENT_COUNT) {
		library_index_t *ix = library_index_open(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
		if (ix) {
			library_index_window(ix, 0, 12, recent_album_row, NULL);
			library_index_close(ix);
		}
	}
	recent_paint();
}

// Collects whatever the worker has finished. Called from the playback
// snapshot, which already runs while this page is the one on screen.
static void recent_poll(void) {
	for (int i = 0; i < recent_filled; i++) {
		recent_t *r = &recents[i];
		if (!r->waiting) continue;
		cover_image_t got = {0};
		bool finished = false;
		if (coverloader_take(RECENT_SLOT_BASE + i, &got, &finished)) {
			cover_free(&r->cover);
			r->cover = got;
			lv_image_set_src(r->image, &r->cover.dsc);
			lv_obj_set_hidden(r->image, false);
			lv_obj_set_hidden(r->note, true);
			r->waiting = false;
		} else if (finished) {
			r->waiting = false;
		}
	}
}

static void recent_cb(lv_event_t *event) {
	if (gesture_active()) return;
	recent_t *r = lv_event_get_user_data(event);
	char title[256] = "";
	library_album_title(r->value, title, sizeof(title));
	medialist_open(title, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, r->value);
}

static void all_albums_cb(lv_event_t *event) {
	(void)event;
	if (!gesture_active()) open_albums();
}

static void make_recent(lv_obj_t *screen, gui_config_t *cfg, int top) {
	// The section row: the label, and the way into every album.
	lv_obj_t *head = lv_obj_create(screen);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, cfg->screen_width - 2 * 12, 56);
	lv_obj_set_pos(head, 12, top);
	lv_obj_set_scrollable(head, false);
	lv_obj_set_clickable(head, false);
	recent_head = head;
	lv_obj_t *name = label(head, &font_ui_16_bold, true);
	lv_label_set_text(name, tr("home_jump_back_in"));
	lv_obj_set_style_text_letter_space(name, 1, 0);
	lv_obj_set_style_text_opa(name, LV_OPA_80, 0);
	lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);
	lv_obj_t *all = lv_btn_create(head);
	lv_obj_remove_style_all(all);
	lv_obj_set_size(all, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(all, 8, 0);
	lv_obj_align(all, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(all, all_albums_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *all_text = label(all, &font_ui_18_bold, true);
	lv_label_set_text(all_text, tr("home_all_albums"));
	lv_obj_center(all_text);

	int gap = 12;
	int w = (cfg->screen_width - 2 * 12 - 2 * gap) / 3;
	for (int i = 0; i < RECENT_COUNT; i++) {
		recent_t *r = &recents[i];
		lv_obj_t *tile = lv_btn_create(screen);
		lv_obj_remove_style_all(tile);
		lv_obj_set_size(tile, w, w + 60);
		lv_obj_set_pos(tile, 12 + i * (w + gap), top + 56);
		lv_obj_set_scrollable(tile, false);
		lv_obj_set_style_opa(tile, LV_OPA_70, LV_STATE_PRESSED);
		lv_obj_add_event_cb(tile, recent_cb, LV_EVENT_CLICKED, r);
		player_sheet_attach_drag(tile, true);
		r->tile = tile;

		lv_obj_t *box = lv_obj_create(tile);
		lv_obj_remove_style_all(box);
		lv_obj_add_style(box, &theme_style_card, 0);
		lv_obj_set_size(box, w, w);
		lv_obj_set_style_radius(box, 10, 0); // the picture rounds itself: see the card's cover
		lv_obj_set_clickable(box, false);
		lv_obj_set_scrollable(box, false);
		r->box = box;
		r->note = lv_image_create(box);
		lv_image_set_src(r->note, &icon_music_note);
		lv_obj_add_style(r->note, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(r->note, LV_OPA_30, 0);
		lv_obj_center(r->note);
		r->image = lv_image_create(box);
		lv_obj_set_size(r->image, w, w);
		lv_obj_set_style_radius(r->image, 10, 0);
		lv_image_set_inner_align(r->image, LV_IMAGE_ALIGN_STRETCH);
		lv_obj_set_hidden(r->image, true);

		r->title = label(tile, &font_ui_20, false);
		lv_obj_set_width(r->title, w);
		lv_obj_set_height(r->title, lv_font_get_line_height(&font_ui_20));
		lv_label_set_long_mode(r->title, LV_LABEL_LONG_DOT);
		lv_obj_set_pos(r->title, 0, w + 6);
		r->artist = label(tile, &font_ui_16, true);
		lv_obj_set_width(r->artist, w);
		lv_obj_set_height(r->artist, lv_font_get_line_height(&font_ui_16));
		lv_label_set_long_mode(r->artist, LV_LABEL_LONG_DOT);
		lv_obj_set_pos(r->artist, 0, w + 8 + lv_font_get_line_height(&font_ui_20));
		lv_obj_set_hidden(tile, true);
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

static void control_centre_cb(lv_event_t *event) {
	(void)event;
	if (!gesture_active()) quickpanel_open();
}

void main_menu_init(gui_config_t *cfg) {
	lv_obj_add_style(main_menu_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(main_menu_screen, false);

	// Header: the day and date, and the control centre in the corner.
	const int header_h = 84;
	int y = cfg->top_bar_height;
	weekday_label = label(main_menu_screen, &font_ui_16_bold, false);
	lv_obj_add_style(weekday_label, &theme_style_nav_icon, 0);
	lv_obj_set_style_text_color(weekday_label, theme()->accent, 0);
	lv_obj_set_style_text_letter_space(weekday_label, 1, 0);
	lv_obj_align(weekday_label, LV_ALIGN_TOP_LEFT, 20, y + 12);
	date_label = label(main_menu_screen, &font_ui_32_bold, false);
	lv_obj_align(date_label, LV_ALIGN_TOP_LEFT, 20, y + 34);
	lv_obj_t *cc = round_button(main_menu_screen, 72, NULL, 0);
	lv_obj_add_event_cb(cc, control_centre_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(cc, LV_ALIGN_TOP_RIGHT, -8, y + 6);
	glyph(cc, &icon_sliders, 34);
	y += header_h;

	int tabs = navbar_attach(main_menu_screen, cfg, NAVBAR_HOME, false);
	int bottom = cfg->screen_height - tabs - 12;

	(void)bottom;
	int card_h = make_player(main_menu_screen, cfg, y, 124);
	make_recent(main_menu_screen, cfg, y + card_h);

	make_drawer(cfg);
	lv_obj_add_event_cb(main_menu_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
