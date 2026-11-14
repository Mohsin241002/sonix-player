#include "navbar.h"

#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <string.h>

#include "src/gui/ebook/ebookpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/music.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

// ---------------------------------------------------------------------------
// The artwork thumbnail
//
// Every mini-player shows the same cover, so they share one small RGB565 copy
// of it (52 x 52, 5.4 KiB), made from the player's own decoded cover when the
// track changes. No decoder, file or full-size buffer is touched here.
// ---------------------------------------------------------------------------

#define THUMB 52
static LV_ATTRIBUTE_MEM_ALIGN uint16_t thumb_pixels[THUMB * THUMB];
static lv_image_dsc_t thumb = {
	.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = THUMB, .h = THUMB,
			   .stride = THUMB * 2},
	.data_size = sizeof(thumb_pixels),
	.data = (const uint8_t *)thumb_pixels,
};
static uint64_t thumb_identity;

// True when the thumbnail holds the current cover.
static bool thumb_valid;

static bool update_thumb_inner(void);

// Brought up to date on the way out: the pages with no mini-player of their
// own (Home, and the control centre over it) ask for the picture too.
const lv_image_dsc_t *navbar_thumb(void) {
	thumb_valid = update_thumb_inner();
	return thumb_valid ? &thumb : NULL;
}

static bool update_thumb(void) {
	thumb_valid = update_thumb_inner();
	return thumb_valid;
}

static bool update_thumb_inner(void) {
	uint64_t identity = player_cover_identity();
	const lv_image_dsc_t *src = identity ? player_cover_image() : NULL;
	bool valid = src && src->data && src->header.cf == LV_COLOR_FORMAT_RGB565 && src->header.w && src->header.h &&
				 src->header.stride >= src->header.w * 2 &&
				 src->data_size >= (uint32_t)src->header.stride * src->header.h;
	if (!valid) {
		thumb_identity = 0;
		return false;
	}
	if (identity == thumb_identity) {
		return true;
	}
	for (unsigned y = 0; y < THUMB; y++) {
		const uint8_t *row = src->data + (y * src->header.h / THUMB) * src->header.stride;
		for (unsigned x = 0; x < THUMB; x++) {
			memcpy(&thumb_pixels[y * THUMB + x], row + (x * src->header.w / THUMB) * 2, 2);
		}
	}
	thumb_identity = identity;
	lv_image_cache_drop(&thumb);
	return true;
}

// ---------------------------------------------------------------------------
// Mini-players
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *screen;
	lv_obj_t *bar;
	lv_obj_t *cover;
	lv_obj_t *note; // shown instead of the cover when there is none
	lv_obj_t *title;
	lv_obj_t *artist;
	lv_obj_t *play_icon;
	lv_obj_t *buttons[2];
	uint64_t displayed_cover;
} mini_t;

#define MAX_MINIS 24
static mini_t minis[MAX_MINIS];
static int mini_count;
static mini_t *shared_mini;

static int tab_bar_count;
static void tabs_refresh_theme(void);

static bool gesture_active(void) { return player_sheet_drag_active() || switcher_back_drag_active(); }

static void set_text_if_changed(lv_obj_t *label, const char *text) {
	if (strcmp(lv_label_get_text(label), text) != 0) {
		lv_label_set_text(label, text);
	}
}

static void paint_mini(mini_t *m, const device_state_t *state) {
	bool loaded = state->live || state->current_file[0];
	const char *file = strrchr(state->current_file, '/');
	const char *title = state->metadata.title;
	if (!title[0]) {
		title = state->current_file[0] ? (file ? file + 1 : state->current_file) : tr("player_no_track_loaded");
	}
	set_text_if_changed(m->title, title);
	set_text_if_changed(m->artist, loaded ? state->metadata.artist : "");

	const lv_image_dsc_t *glyph = state->status == AUDIO_STATUS_PLAYING ? &icon_pause : &icon_play;
	if (lv_image_get_src(m->play_icon) != glyph) {
		lv_image_set_src(m->play_icon, glyph);
	}
	for (int i = 0; i < 2; i++) {
		lv_obj_set_state(m->buttons[i], LV_STATE_DISABLED, !loaded);
	}

	bool art = loaded && update_thumb();
	uint64_t identity = art ? thumb_identity : 0;
	if (identity != m->displayed_cover) {
		lv_obj_set_hidden(m->cover, !art);
		lv_obj_set_hidden(m->note, art);
		if (art) {
			lv_obj_invalidate(m->cover);
		}
		m->displayed_cover = identity;
	}
}

void navbar_refresh_player(const device_state_t *state) {
	if (player_sheet_is_open()) {
		return;
	}
	lv_obj_t *shown = lv_screen_active();
	for (int i = 0; i < mini_count; i++) {
		if (minis[i].screen == shown && lv_obj_is_visible(minis[i].bar)) {
			paint_mini(&minis[i], state);
		}
	}
}

static void refresh_now(void) {
	device_state_t state;
	device_state_get(&state);
	navbar_refresh_player(&state);
}

static void mini_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_now();
}

static void mini_open_cb(lv_event_t *e) {
	(void)e;
	if (!gesture_active()) {
		switch_screen(player_screen);
	}
}

static void mini_button_cb(lv_event_t *e) {
	if (gesture_active()) {
		return;
	}
	if (lv_event_get_user_data(e)) {
		player_key_next();
	} else {
		player_key_play_pause();
	}
	refresh_now();
}

static lv_obj_t *mini_button(lv_obj_t *parent, const lv_image_dsc_t *icon, int size, bool next) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_remove_style_all(button);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_size(button, 72, NAVBAR_MINI_HEIGHT - 4);
	lv_obj_set_style_radius(button, 16, 0);
	lv_obj_set_style_opa(button, LV_OPA_30, LV_STATE_DISABLED);
	lv_obj_add_event_cb(button, mini_button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)next);
	lv_obj_t *glyph = lv_image_create(button);
	lv_image_set_src(glyph, icon);
	lv_obj_set_size(glyph, size, size);
	lv_image_set_inner_align(glyph, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_add_style(glyph, &theme_style_icon, 0);
	lv_obj_center(glyph);
	return button;
}

static int build_mini(lv_obj_t *screen, gui_config_t *cfg, int bottom) {
	if (mini_count == 0 && tab_bar_count == 0) {
		theme_register_refresh(tabs_refresh_theme);
	}
	if (mini_count >= MAX_MINIS) {
		LV_LOG_WARN("navbar: no room for another mini-player");
		return 0;
	}
	mini_t *m = &minis[mini_count++];
	m->screen = screen;

	lv_obj_t *bar = lv_obj_create(screen);
	m->bar = bar;
	lv_obj_remove_style_all(bar);
	lv_obj_add_style(bar, &theme_style_card, 0);
	lv_obj_add_style(bar, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_size(bar, cfg->screen_width, NAVBAR_MINI_HEIGHT);
	lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -bottom);
	// The amber rule along the top says "this is what is playing" at a glance.
	lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(bar, 3, 0);
	lv_obj_set_style_border_color(bar, theme()->accent, 0);
	lv_obj_set_style_pad_left(bar, 12, 0);
	lv_obj_set_style_pad_gap(bar, 12, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(bar, false);
	lv_obj_add_event_cb(bar, mini_open_cb, LV_EVENT_CLICKED, NULL);
	player_sheet_attach_drag(bar, true);

	lv_obj_t *frame = lv_obj_create(bar);
	lv_obj_remove_style_all(frame);
	lv_obj_add_style(frame, &theme_style_card_pressed, 0);
	lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
	lv_obj_set_size(frame, THUMB, THUMB);
	lv_obj_set_style_radius(frame, 8, 0);
	lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_scrollable(frame, false);
	// Rounded by the image itself (LVGL clips an image to its own radius as it
	// draws), not by clip_corner on the frame: on an object with children that
	// renders the frame into an offscreen layer on every redraw of it.
	m->cover = lv_image_create(frame);
	lv_obj_set_style_radius(m->cover, 8, 0);
	lv_obj_set_hidden(m->cover, true);
	lv_image_set_src(m->cover, &thumb);
	m->note = lv_image_create(frame);
	lv_image_set_src(m->note, &icon_music_note);
	lv_obj_set_size(m->note, 30, 30);
	lv_image_set_inner_align(m->note, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_add_style(m->note, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(m->note, LV_OPA_50, 0);
	lv_obj_center(m->note);

	lv_obj_t *text = lv_obj_create(bar);
	lv_obj_remove_style_all(text);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_scrollable(text, false);
	m->title = lv_label_create(text);
	lv_label_set_text_static(m->title, "");
	lv_obj_add_style(m->title, &theme_style_text, 0);
	lv_obj_set_style_text_font(m->title, &font_ui_22, 0);
	lv_obj_set_width(m->title, lv_pct(100));
	lv_label_set_long_mode(m->title, LV_LABEL_LONG_DOT);
	lv_obj_set_height(m->title, lv_font_get_line_height(&font_ui_22));
	m->artist = lv_label_create(text);
	lv_label_set_text_static(m->artist, "");
	lv_obj_add_style(m->artist, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(m->artist, &font_ui_18, 0);
	lv_obj_set_width(m->artist, lv_pct(100));
	lv_label_set_long_mode(m->artist, LV_LABEL_LONG_DOT);
	lv_obj_set_height(m->artist, lv_font_get_line_height(&font_ui_18));

	m->buttons[0] = mini_button(bar, &icon_play, 40, false);
	m->play_icon = lv_obj_get_child(m->buttons[0], 0);
	m->buttons[1] = mini_button(bar, &icon_skip_forward, 30, true);

	lv_obj_add_event_cb(screen, mini_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	return NAVBAR_MINI_HEIGHT;
}

// The border colour is set by hand above, so it is repainted after an accent
// change like the rest of the hand-set colours.
static void minis_refresh_theme(void) {
	for (int i = 0; i < mini_count; i++) {
		lv_obj_set_style_border_color(minis[i].bar, theme()->accent, 0);
	}
}

// ---------------------------------------------------------------------------
// Tab bars
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *icons[5];
	lv_obj_t *labels[5];
	navbar_tab_t active;
} tabs_t;

#define MAX_TAB_BARS 8
static tabs_t tab_bars[MAX_TAB_BARS];

static void go_home(void) {
	screen_history_reset();
	switch_screen_no_history(main_menu_screen);
}

static void go_screen(lv_obj_t *screen) {
	if (lv_screen_active() == screen) {
		return;
	}
	// A tab is a place, not a step: going there forgets the trail, so Back
	// from a tab's page leads Home rather than through every tab visited.
	screen_history_reset();
	switch_screen(screen);
}

static void tab_cb(lv_event_t *e) {
	if (gesture_active()) {
		return;
	}
	switch ((navbar_tab_t)(intptr_t)lv_event_get_user_data(e)) {
	case NAVBAR_HOME:
		go_home();
		break;
	case NAVBAR_LIBRARY:
		go_screen(music_screen);
		break;
	case NAVBAR_SEARCH:
		search_open();
		break;
	case NAVBAR_BOOKS:
		screen_history_reset();
		ebookpage_open();
		break;
	case NAVBAR_MORE:
		go_screen(morepage_screen);
		break;
	default:
		break;
	}
}

static void paint_tabs(tabs_t *t) {
	for (int i = 0; i < 5; i++) {
		lv_color_t c = i == (int)t->active ? theme()->accent : theme()->text_secondary;
		lv_obj_set_style_image_recolor(t->icons[i], c, 0);
		lv_obj_set_style_text_color(t->labels[i], c, 0);
	}
}

static void tabs_refresh_theme(void) {
	for (int i = 0; i < tab_bar_count; i++) {
		paint_tabs(&tab_bars[i]);
	}
	minis_refresh_theme();
}

static int build_tabs(lv_obj_t *screen, gui_config_t *cfg, navbar_tab_t active) {
	static const struct {
		const lv_image_dsc_t *icon;
		const char *label;
	} TABS[5] = {
		{&icon_tab_home, "nav_home"},
		{&icon_tab_library, "nav_library"},
		{&icon_tab_search, "nav_search"},
		{&icon_tab_books, "books"},
		{&icon_tab_more, "more"},
	};

	if (tab_bar_count >= MAX_TAB_BARS) {
		LV_LOG_WARN("navbar: no room for another tab bar");
		return 0;
	}
	if (tab_bar_count == 0 && mini_count == 0) {
		theme_register_refresh(tabs_refresh_theme);
	}
	tabs_t *t = &tab_bars[tab_bar_count++];
	t->active = active;

	lv_obj_t *bar = lv_obj_create(screen);
	lv_obj_remove_style_all(bar);
	lv_obj_add_style(bar, &theme_style_screen, 0);
	lv_obj_set_size(bar, cfg->screen_width, NAVBAR_TABS_HEIGHT);
	lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(bar, 1, 0);
	lv_obj_set_style_border_color(bar, theme()->surface_pressed, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_scrollable(bar, false);

	for (int i = 0; i < 5; i++) {
		lv_obj_t *tab = lv_btn_create(bar);
		lv_obj_remove_style_all(tab);
		lv_obj_add_style(tab, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_height(tab, lv_pct(100));
		lv_obj_set_flex_grow(tab, 1);
		lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_gap(tab, 4, 0);
		lv_obj_add_event_cb(tab, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		t->icons[i] = lv_image_create(tab);
		lv_image_set_src(t->icons[i], TABS[i].icon);
		lv_obj_set_style_image_recolor_opa(t->icons[i], LV_OPA_COVER, 0);
		t->labels[i] = lv_label_create(tab);
		lv_label_set_text(t->labels[i], tr(TABS[i].label));
		lv_obj_set_style_text_font(t->labels[i], &font_ui_16, 0);
	}
	paint_tabs(t);
	return NAVBAR_TABS_HEIGHT;
}

int navbar_attach(lv_obj_t *screen, gui_config_t *cfg, navbar_tab_t active, bool mini) {
	int used = build_tabs(screen, cfg, active);
	if (mini) {
		used += build_mini(screen, cfg, used);
	}
	return used;
}

int navbar_attach_mini(lv_obj_t *screen, gui_config_t *cfg) { return build_mini(screen, cfg, 0); }

static void shared_mini_loaded_cb(lv_event_t *e) {
	lv_obj_t *screen = lv_event_get_current_target(e);
	if (shared_mini->screen != screen) {
		lv_obj_set_parent(shared_mini->bar, screen);
		lv_obj_align(shared_mini->bar, LV_ALIGN_BOTTOM_MID, 0, 0);
		shared_mini->screen = screen;
	}
	lv_obj_move_foreground(shared_mini->bar);
	refresh_now();
}

int navbar_use_shared_mini(lv_obj_t *screen, gui_config_t *cfg) {
	if (!shared_mini) {
		int index = mini_count;
		if (!build_mini(screen, cfg, 0)) {
			return 0;
		}
		shared_mini = &minis[index];
		lv_obj_remove_event_cb(screen, mini_loaded_cb);
	}
	lv_obj_add_event_cb(screen, shared_mini_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	return NAVBAR_MINI_HEIGHT;
}
