#include "screensaver.h"
#include "src/system/bluetooth/bluetooth.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/clocklabel.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"
#include "src/system/device/power.h"
#include "src/system/device/screensaverpics.h"

// Movement that turns a press into a drag, and how long the slide that follows
// the release lasts. The dismiss threshold itself is a quarter of the screen,
// applied in drag_cb; short of it the panel slides back down, like the control
// centre.
#define DRAG_ENGAGE_PX 8
#define DISMISS_ANIM_MS 180

static lv_obj_t *saver;		 // the whole thing, full screen on the top layer
static lv_obj_t *cover_img;	 // the artwork, filling what is above the strip
static lv_obj_t *cover_placeholder; // the note, for a track that has none
static lv_obj_t *info_strip; // the blurred block along the bottom
static lv_obj_t *cover_box;	 // the artwork's frame, everything above the strip
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *clock_label;
static lv_timer_t *clock_timer;

static bool enabled;
static int screen_w, screen_h;
static int strip_h;

// What the strip falls back to when there is no artwork to size it from, and
// the height it always has over a picture from the folder -- there the block
// is built to order rather than borrowed, so there is nothing to measure.
#define DEFAULT_STRIP_H 250

// ---------------------------------------------------------------------------
// Pictures from the card
//
// The other thing the screensaver can show: one of the JPEGs in the card's
// Screensaver folder, a different one each time the screen lights up. Which
// files qualify and which one comes up is screensaverpics.h; what is left here
// is the drawing and the pixels it owns.
//
// Nothing is decoded on this thread. The picture for the next wake is asked of
// the cover loader as the panel goes dark and is waiting, decoded, by the time
// the screen comes back -- so a wake costs a pointer swap rather than a card
// read, a JPEG and a blur with the interface thread standing still and the
// audio decoder sharing the one core with it.
// ---------------------------------------------------------------------------

static screensaver_source_t source;
// The picture on screen and the blurred block over its lower part. Owned here,
// not by the player: freed here whenever a different one is chosen or the
// screensaver goes away.
static cover_image_t image_pic, image_strip;
static char image_file[512]; // what image_pic was built from

// The one being prepared for the next wake: `next_file` while it is out with
// the loader, and the pixels once they have been collected.
static cover_image_t next_pic, next_strip;
static char next_file[512];
static bool next_ready;	  // next_pic/next_strip hold a collected picture
static bool next_pending; // a request is out with the loader

// Resizes the strip and gives the picture whatever it should have.
//
// LVGL draws a background image at its own size, centred, and paints whatever
// is left over in the background colour, so a strip even a few pixels taller
// than the blurred block would show a black bar above it and another below.
// The height is therefore taken from the picture, leaving nothing to paint.
//
// Over an album cover the picture's box stops where the strip starts. Over a
// photograph from the folder it is the whole screen: there the strip is drawn
// ON TOP of a picture that fills the panel, which is what "the blurred part
// hides the bottom of the image" means.
//
// Cheap, and a no-op unless something has actually changed.
static int cover_box_h;

static void set_strip_height(int height) {
	if (height <= 0 || height >= screen_h) {
		return;
	}
	if (height != strip_h) {
		strip_h = height;
		lv_obj_set_height(info_strip, strip_h);
		lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	}
	int want = source == SCREENSAVER_SOURCE_IMAGES ? screen_h : screen_h - strip_h;
	if (want != cover_box_h) {
		cover_box_h = want;
		lv_obj_set_height(cover_box, cover_box_h);
	}
}

bool screensaver_is_visible(void) { return saver && !lv_obj_is_hidden(saver); }
bool screensaver_get_enabled(void) { return enabled; }

screensaver_source_t screensaver_source(void) { return source; }

static void request_next(void);
static void drop_next(void);

void screensaver_set_source(screensaver_source_t chosen) {
	source = chosen == SCREENSAVER_SOURCE_IMAGES ? SCREENSAVER_SOURCE_IMAGES : SCREENSAVER_SOURCE_ALBUM;
	config_set_int("screen", "screensaver_source", (int)source);
	config_save();
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// Asked for now rather than at the first screen-off, so the first wake
		// after the option is chosen already has a picture to show.
		request_next();
	} else {
		drop_next();
	}
}

void screensaver_set_card_root(const char *root) { screensaverpics_set_root(root); }

bool screensaver_has_images(void) { return screensaverpics_any(screen_w, screen_h); }

// Picks the next picture and puts it out to decode. Called as the panel goes
// dark, which is the whole point: the card read and the JPEG happen on the
// loader thread at background priority over a screen nobody is looking at.
//
// A no-op while one is already in hand or in flight, so the repeated wake-hook
// calls cost nothing.
static void request_next(void) {
	if (source != SCREENSAVER_SOURCE_IMAGES || next_ready || next_pending) {
		return;
	}
	char chosen[sizeof(next_file)];
	if (!screensaverpics_pick(image_file, screen_w, screen_h, chosen, sizeof(chosen))) {
		// The folder gave nothing at all -- emptied, or the card taken out.
		// What is already up stays up: it is still a picture, and black is not.
		return;
	}
	snprintf(next_file, sizeof(next_file), "%s", chosen);
	next_pending = true;
	coverloader_request_screensaver(next_file, screen_w, screen_h, screen_w, DEFAULT_STRIP_H);
}

// Takes delivery of a finished decode, if there is one. Cheap and non-blocking:
// called at every wake and on the tick while the screensaver is up.
static void collect_next(void) {
	if (!next_pending) {
		return;
	}
	bool finished = false;
	cover_image_t pic = {0}, strip = {0};
	bool found = coverloader_take_screensaver(&pic, &strip, &finished);
	if (!finished) {
		return;
	}
	next_pending = false;
	if (!found) {
		// It passed the header check and still would not decode. Forgotten, so
		// the next request picks another rather than retrying this one.
		next_file[0] = '\0';
		return;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_pic = pic;
	next_strip = strip;
	next_ready = true;
}

static void drop_next(void) {
	if (next_pending) {
		coverloader_release_screensaver();
		next_pending = false;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_ready = false;
	next_file[0] = '\0';
}

static void drop_image(void) {
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_file[0] = '\0';
}

void screensaver_set_enabled(bool on) {
	enabled = on;
	config_set_bool("screen", "screensaver", on);
	config_save();
	if (!on) {
		if (screensaver_is_visible()) {
			screensaver_hide(); // frees what is held on its way out
		} else {
			drop_next();
		}
	}
}

// ---------------------------------------------------------------------------
// what it shows
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The lock screen
//
// With the album as its source the screensaver is the design's lock screen:
// the date and a very large clock at the top, and at the bottom a card with
// the sleeve, the names, the format, where the track has got to and the three
// transport buttons. It covers the older full-bleed picture, which the Images
// source still uses.
// ---------------------------------------------------------------------------

static lv_obj_t *lock;		   // the whole lock screen, over the picture and the strip
static lv_obj_t *lock_date;
static lv_obj_t *lock_clock;
static lv_obj_t *lock_cover;
static lv_obj_t *lock_note;
static lv_obj_t *lock_title;
static lv_obj_t *lock_artist;
static lv_obj_t *lock_badge;
static lv_obj_t *lock_elapsed;
static lv_obj_t *lock_total;
static lv_obj_t *lock_bar;
static lv_obj_t *lock_play_icon;
static bool lock_dragged; // the press in progress has become the swipe that unlocks
static int lock_day = -1;

static void lock_time(lv_obj_t *label, double seconds) {
	unsigned value = seconds > 0 && seconds < 360000 ? (unsigned)seconds : 0;
	char text[24];
	if (value >= 3600) {
		snprintf(text, sizeof(text), "%u:%02u:%02u", value / 3600, value / 60 % 60, value % 60);
	} else {
		snprintf(text, sizeof(text), "%u:%02u", value / 60, value % 60);
	}
	if (strcmp(lv_label_get_text(label), text) != 0) {
		lv_label_set_text(label, text);
	}
}

// What moves from second to second: the date at midnight, the line, the two
// clocks either side of it and the play glyph.
static void lock_tick(void) {
	if (!lock || lv_obj_is_hidden(lock)) {
		return;
	}
	clocklabel_refresh(lock_clock);

	int day = 0;
	char date[64] = "";
	if (clock_is_set()) {
		time_t now = time(NULL);
		struct tm local;
		localtime_r(&now, &local);
		day = local.tm_yday + 1;
		if (day != lock_day) {
			strftime(date, sizeof(date), "%A %e %B", &local);
			for (char *c = date; *c; c++) {
				if (*c >= 'a' && *c <= 'z') {
					*c = (char)(*c - 32);
				}
			}
			lv_label_set_text(lock_date, date);
		}
	} else if (lock_day != 0) {
		lv_label_set_text(lock_date, "");
	}
	lock_day = day;

	device_state_t state;
	device_state_get(&state);
	int progress = !state.live && state.progress_total_secs > 0
					   ? (int)(1000.0 * state.progress_current_secs / state.progress_total_secs)
					   : 0;
	if (progress < 0) {
		progress = 0;
	}
	if (progress > 1000) {
		progress = 1000;
	}
	if (lv_bar_get_value(lock_bar) != progress) {
		lv_bar_set_value(lock_bar, progress, LV_ANIM_OFF);
	}
	lock_time(lock_elapsed, state.live ? 0 : state.progress_current_secs);
	lock_time(lock_total, state.live ? 0 : state.progress_total_secs);
	const lv_image_dsc_t *glyph = state.status == AUDIO_STATUS_PLAYING ? &icon_pause_large : &icon_play_large;
	if (lv_image_get_src(lock_play_icon) != glyph) {
		lv_image_set_src(lock_play_icon, glyph);
	}
	const char *format = player_format_text();
	bt_device_t output;
	if (bluetooth_connected_device(&output) && output.name[0]) {
		format = output.name;
	}
	if (strcmp(lv_label_get_text(lock_badge), format) != 0) {
		lv_label_set_text(lock_badge, format);
	}
	lv_obj_set_hidden(lock_badge, !format[0]);
}

// The sleeve on the card: the player's own picture, scaled down. Never kept
// across a track change -- screensaver_release_artwork() blanks it first.
static void lock_set_cover(const lv_image_dsc_t *cover) {
	if (!lock_cover) {
		return;
	}
	if (cover && cover->header.w) {
		lv_image_set_src(lock_cover, cover);
		lv_obj_set_hidden(lock_cover, false);
		lv_obj_set_hidden(lock_note, true);
	} else {
		lv_image_set_src(lock_cover, NULL);
		lv_obj_set_hidden(lock_cover, true);
		lv_obj_set_hidden(lock_note, false);
	}
}

static void lock_transport_cb(lv_event_t *e) {
	if (lock_dragged) {
		return; // the end of the swipe that unlocks, not a tap
	}
	int action = (int)(intptr_t)lv_event_get_user_data(e);
	if (action < 0) {
		player_key_prev();
	} else if (action > 0) {
		player_key_next();
	} else {
		player_key_play_pause();
	}
	lock_tick();
}

static lv_obj_t *lock_label(lv_obj_t *parent, const lv_font_t *font, bool dim) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, "");
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	return label;
}

static void lock_build(void) {
	lock = lv_obj_create(saver);
	lv_obj_remove_style_all(lock);
	lv_obj_add_style(lock, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(lock, LV_OPA_COVER, 0);
	lv_obj_set_size(lock, screen_w, screen_h);
	lv_obj_set_pos(lock, 0, 0);
	lv_obj_set_scrollable(lock, false);
	lv_obj_set_clickable(lock, false); // presses reach the saver, where the swipe lives
	lv_obj_set_hidden(lock, true);

	lock_date = lock_label(lock, &font_ui_18_bold, false);
	lv_obj_set_style_text_color(lock_date, theme()->accent, 0);
	lv_obj_set_style_text_letter_space(lock_date, 1, 0);
	lv_obj_set_pos(lock_date, 24, 64);

	lock_clock = clocklabel_create(lock, &font_ui_120_bold, 120, &font_ui_32_bold, 32);
	lv_obj_add_style(lock_clock, &theme_style_text, 0);
	lv_obj_set_pos(lock_clock, 20, 88);

	// The card.
	const int pad = 16, art = 112, card_w = screen_w - 24;
	const int card_h = pad + art + 14 + 24 + 14 + 96 + 12;
	lv_obj_t *card = lv_obj_create(lock);
	lv_obj_remove_style_all(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 20, 0);
	lv_obj_set_size(card, card_w, card_h);
	lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -88);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, false);

	lv_obj_t *box = lv_obj_create(card);
	lv_obj_remove_style_all(box);
	lv_obj_add_style(box, &theme_style_card_pressed, 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_set_size(box, art, art);
	lv_obj_set_pos(box, pad, pad);
	lv_obj_set_style_radius(box, 10, 0);
	lv_obj_set_style_clip_corner(box, true, 0);
	lv_obj_set_clickable(box, false);
	lv_obj_set_scrollable(box, false);
	lock_note = lv_image_create(box);
	lv_image_set_src(lock_note, &icon_music_note);
	lv_obj_add_style(lock_note, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(lock_note, LV_OPA_30, 0);
	lv_obj_center(lock_note);
	lock_cover = lv_image_create(box);
	lv_obj_set_size(lock_cover, art, art);
	lv_image_set_inner_align(lock_cover, LV_IMAGE_ALIGN_STRETCH);
	lv_obj_set_hidden(lock_cover, true);

	int text_x = pad + art + 16, text_w = card_w - text_x - pad;
	lv_obj_t *text = lv_obj_create(card);
	lv_obj_remove_style_all(text);
	lv_obj_set_size(text, text_w, art);
	lv_obj_set_pos(text, text_x, pad);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(text, 3, 0);
	lv_obj_set_clickable(text, false);
	lv_obj_set_scrollable(text, false);
	lock_title = lock_label(text, &font_ui_28_bold, false);
	lv_obj_set_width(lock_title, text_w);
	lv_obj_set_height(lock_title, lv_font_get_line_height(&font_ui_28_bold));
	lv_label_set_long_mode(lock_title, LV_LABEL_LONG_DOT);
	lock_artist = lock_label(text, &font_ui_20, true);
	lv_obj_set_width(lock_artist, text_w);
	lv_obj_set_height(lock_artist, lv_font_get_line_height(&font_ui_20));
	lv_label_set_long_mode(lock_artist, LV_LABEL_LONG_DOT);
	lock_badge = lock_label(text, &font_ui_16_bold, true);
	lv_obj_set_style_border_width(lock_badge, 2, 0);
	lv_obj_set_style_border_color(lock_badge, theme()->surface_pressed, 0);
	lv_obj_set_style_radius(lock_badge, 6, 0);
	lv_obj_set_style_pad_hor(lock_badge, 8, 0);
	lv_obj_set_style_pad_ver(lock_badge, 2, 0);
	lv_obj_set_style_margin_top(lock_badge, 6, 0);
	lv_obj_set_style_max_width(lock_badge, text_w, 0);
	lv_label_set_long_mode(lock_badge, LV_LABEL_LONG_DOT);

	int y = pad + art + 14;
	lock_elapsed = lock_label(card, &font_ui_18, true);
	lv_obj_set_width(lock_elapsed, 60);
	lv_obj_align(lock_elapsed, LV_ALIGN_TOP_LEFT, pad, y);
	lock_total = lock_label(card, &font_ui_18, true);
	lv_obj_set_width(lock_total, 60);
	lv_obj_set_style_text_align(lock_total, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_align(lock_total, LV_ALIGN_TOP_RIGHT, -pad, y);
	lock_bar = lv_bar_create(card);
	lv_obj_remove_style_all(lock_bar);
	lv_obj_set_size(lock_bar, card_w - 2 * pad - 2 * 66, 6);
	lv_obj_align(lock_bar, LV_ALIGN_TOP_MID, 0, y + 9);
	lv_bar_set_range(lock_bar, 0, 1000);
	lv_obj_set_style_radius(lock_bar, 3, LV_PART_MAIN);
	lv_obj_set_style_radius(lock_bar, 3, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(lock_bar, theme()->surface_pressed, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(lock_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_add_style(lock_bar, &theme_style_accent_bg, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(lock_bar, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_set_clickable(lock_bar, false);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, card_w - 2 * pad, 96);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y + 24 + 14);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 28, 0);
	lv_obj_set_clickable(row, false);
	lv_obj_set_scrollable(row, false);
	for (int action = -1; action <= 1; action++) {
		lv_obj_t *button = lv_btn_create(row);
		lv_obj_remove_style_all(button);
		lv_obj_set_size(button, action ? 88 : 96, action ? 88 : 96);
		lv_obj_set_style_radius(button, action ? 18 : LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_PRESSED);
		// The swipe that unlocks can start on a button too.
		lv_obj_set_event_bubble(button, true);
		lv_obj_add_event_cb(button, lock_transport_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);
		lv_obj_t *icon = lv_image_create(button);
		lv_obj_center(icon);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		if (action) {
			lv_image_set_src(icon, action < 0 ? &icon_skip_back_large : &icon_skip_forward_large);
			lv_obj_add_style(icon, &theme_style_icon, 0);
		} else {
			lv_image_set_src(icon, &icon_play_large);
			lv_obj_add_style(icon, &theme_style_icon, 0);
			lock_play_icon = icon;
		}
	}

	// The way out, in words under the chevron.
	lv_obj_t *hint = lock_label(lock, &font_ui_18, true);
	lv_label_set_text(hint, tr("lock_swipe_up"));
	lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);
}

static void refresh_clock(void) {
	clocklabel_refresh(clock_label);
	lock_tick();
}

// What refresh_content() last drew, so the tick below can tell an idle moment
// from a track that changed while the screensaver was up.
static char shown_file[512];
static const void *shown_cover;

static void refresh_content(void);

// Runs while the screensaver is up. The clock is the obvious reason, but the
// important one is the track: the player frees its artwork at every track
// change and calls screensaver_release_artwork(), which blanks the picture, so
// this check is what puts the new one back.
static void clock_timer_cb(lv_timer_t *timer) {
	(void)timer;

	// Takes delivery of a decode that landed after the wake had already given
	// up on it -- a short dark spell, or a slow card. It is only stored, never
	// swapped in: the picture must not change while it is being looked at.
	collect_next();

	if (!screensaver_is_visible()) {
		refresh_clock();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	const void *cover = player_cover_image();

	const char *now = state.live ? state.metadata.title : state.current_file;
	if (strcmp(now, shown_file) != 0 || cover != shown_cover) {
		refresh_content(); // refreshes the clock on its way through
		return;
	}

	refresh_clock();
}

// Puts the prepared picture on screen. Unlike the album source this owns its
// pixels, so the swap is the careful part: LVGL keeps drawing from the buffer
// it was given, and freeing the old one while it is still the widget's source
// is a use-after-free at the next redraw. Off the screen first, then free.
static void promote_next(void) {
	if (!next_ready) {
		return;
	}
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_pic = next_pic;
	image_strip = next_strip;
	snprintf(image_file, sizeof(image_file), "%s", next_file);

	memset(&next_pic, 0, sizeof(next_pic));
	memset(&next_strip, 0, sizeof(next_strip));
	next_ready = false;
	next_file[0] = '\0';
}

// Draws whatever is currently held. Nothing is decoded here.
static void draw_from_folder(void) {
	set_strip_height(DEFAULT_STRIP_H);

	if (image_pic.pixels) {
		lv_image_set_src(cover_img, &image_pic.dsc);
		// Already built at screen size by cover_load_screensaver_images: no
		// zoom to apply, and none to leave over from the album source.
		lv_image_set_scale(cover_img, 256);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		// An empty folder, or a card that is not there. The note in the middle
		// of a black screen, which is what the album source shows for a track
		// with no artwork -- the same screen, missing the same thing.
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	if (image_strip.pixels) {
		lv_obj_set_style_bg_image_src(info_strip, &image_strip.dsc, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	} else {
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	}
}

// The artwork is the player's own, already decoded for this track: borrowed
// for as long as the screensaver is up and dropped the moment it goes down,
// so a track change while it is hidden can never leave a stale pointer here.
// A picture from the folder is the other way round -- owned here, decoded here
// and freed here.
static void refresh_content(void) {
	device_state_t state;
	device_state_get(&state);

	// Recorded before anything is drawn from it, so what the tick above
	// compares against is exactly what ended up on screen. A live stream has no
	// file, so the station name stands in; without it every radio would look
	// identical to the tick and a change of station would never redraw.
	snprintf(shown_file, sizeof(shown_file), "%s",
			 state.live ? state.metadata.title : state.current_file);

	// The placeholder behind a missing cover says what kind of thing is
	// playing: a note for a track, a radio for a stream.
	if (cover_placeholder) {
		lv_image_set_src(cover_placeholder, state.live ? &icon_radio_player : &icon_music_note);
	}

	if (state.live) {
		scrolltext_set(title_label, state.metadata.title);
		scrolltext_set(artist_label, state.metadata.artist);
	} else if (state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		scrolltext_set(title_label, state.metadata.title[0] ? state.metadata.title
															: (slash ? slash + 1 : state.current_file));
		const char *artist = metadata_shown_artist(&state.metadata);
		scrolltext_set(artist_label, artist);
	} else {
		scrolltext_set(title_label, "");
		scrolltext_set(artist_label, "");
	}

	// Recorded whatever is being drawn: the tick above compares it to notice a
	// track change, and in Images mode a track change still has to redraw the
	// title -- it just must not change the picture.
	shown_cover = player_cover_image();

	if (lock) {
		bool album = source != SCREENSAVER_SOURCE_IMAGES;
		lv_obj_set_hidden(lock, !album);
		if (album) {
			lv_label_set_text(lock_title, lv_label_get_text(title_label));
			lv_label_set_text(lock_artist, lv_label_get_text(artist_label));
			lv_image_set_src(lock_note, state.live ? &icon_radio_player : &icon_music_note);
			lock_set_cover(shown_cover);
			// The full-bleed picture stays down: nothing under the lock screen
			// may point at the player's buffers.
			lv_image_set_src(cover_img, NULL);
			lv_obj_set_hidden(cover_img, true);
			lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
			refresh_clock();
			return;
		}
	}

	if (source == SCREENSAVER_SOURCE_IMAGES) {
		draw_from_folder();
		refresh_clock();
		return;
	}

	// The strip first: the artwork below is scaled to whatever is left above
	// it, so its height has to be settled before that sum is done.
	const lv_image_dsc_t *backdrop = player_backdrop_image();
	// The player's blurred copy is as tall as whatever shows it, and one
	// arrangement shows it behind the whole screen: a strip that tall is the
	// screen, so that one keeps the default height and takes the top of the
	// picture instead.
	int strip_from_backdrop = backdrop ? (int)backdrop->header.h : 0;
	set_strip_height(strip_from_backdrop > 0 && strip_from_backdrop < screen_h ? strip_from_backdrop
																			  : DEFAULT_STRIP_H);

	const lv_image_dsc_t *cover = shown_cover;
	if (cover) {
		lv_image_set_src(cover_img, cover);
		// Fill the whole area above the strip, cropping the overflow: a
		// letterboxed cover with black bars would look like a bug, not a
		// screensaver.
		int32_t zoom_w = (256 * screen_w) / (int32_t)cover->header.w;
		int32_t zoom_h = (256 * (screen_h - strip_h)) / (int32_t)cover->header.h;
		lv_image_set_scale(cover_img, zoom_w > zoom_h ? zoom_w : zoom_h);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	if (backdrop) {
		lv_obj_set_style_bg_image_src(info_strip, backdrop, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	} else {
		// No artwork at all: a plain dark strip carries the text instead.
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	}

	refresh_clock();
}

// ---------------------------------------------------------------------------
// raising and dismissing
// ---------------------------------------------------------------------------

static void anim_y_cb(void *obj, int32_t v);

void screensaver_show(void) {
	if (!saver || !enabled || screensaver_is_visible()) {
		return;
	}

	collect_next();
	promote_next();
	refresh_content();
	lv_anim_delete(saver, anim_y_cb);
	lv_obj_set_y(saver, 0);
	lv_obj_set_hidden(saver, false);
	lv_obj_move_foreground(saver);
	lv_timer_resume(clock_timer);
}

// Everything that has to happen once it is really gone, whether it was
// dismissed by a tap, by a finished swipe, or by switching the option off.
static void finish_hide(void) {
	lv_obj_set_hidden(saver, true);
	lv_obj_set_y(saver, 0);
	lv_timer_pause(clock_timer);
	// Let go of the player's pixels: it frees them at the next track change,
	// and nothing here may still be pointing at them by then. The picture owned
	// here goes too -- about a megabyte, and the device is in use again, which
	// is exactly when that megabyte is worth having back.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	drop_image();
	// The one being prepared goes too. It is another megabyte, the device is in
	// use again, and the next screen-off asks for a fresh one anyway.
	drop_next();
	power_notify_activity(); // dismissing it counts as using the device
}

static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

// Where the slide currently running is headed. Held explicitly rather than read
// back from the final coordinate: LVGL can finish an animation a pixel or four
// short of its end value, which would answer "did it leave?" with a no and
// leave the screensaver technically visible off-screen forever.
static bool sliding_out;

static void slide_done_cb(lv_anim_t *a) {
	(void)a;
	if (sliding_out) {
		finish_hide();
	} else {
		lv_obj_set_y(saver, 0); // exactly home, whatever the last frame did
	}
}

// Animates the last stretch after the finger lets go: up and away, or back
// down into place.
static void slide_to(int y) {
	lv_anim_delete(saver, anim_y_cb);
	sliding_out = (y != 0);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, saver);
	lv_anim_set_exec_cb(&a, anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(saver), y);
	lv_anim_set_duration(&a, DISMISS_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, slide_done_cb);
	lv_anim_start(&a);
}

bool screensaver_prepare(void) {
	if (!saver || !enabled) {
		return false;
	}
	// Called once as the panel goes dark and again as it lights up, and the two
	// do different work. Going dark is when the next picture is asked for: the
	// screen is about to be off, the loader thread has it to itself, and nobody
	// is waiting. Lighting up is when it is collected and swapped in -- no card,
	// no decode, nothing between the button and the backlight.
	bool waking = power_screen_is_on();

	if (screensaver_is_visible()) {
		collect_next();
		if (waking) {
			promote_next();
		}
		refresh_content();
	} else {
		screensaver_show(); // collects and promotes on its way through
	}

	if (!waking) {
		request_next();
	}
	return screensaver_is_visible();
}

void screensaver_release_artwork(void) {
	if (!cover_img) {
		return;
	}
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// Nothing here is the player's: it can free its artwork without this
		// screen losing anything, and blanking the picture would put a black
		// screen up for no reason.
		return;
	}

	// Whatever was on screen is gone; the tick above has to see a difference
	// so it puts the new track's picture up rather than leaving black.
	shown_cover = NULL;
	shown_file[0] = '\0';
	// Whatever happens next, nothing here may still be pointing at the
	// player's buffers when it frees them.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_hidden(cover_img, true);
	lv_obj_set_hidden(cover_placeholder, false);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	lock_set_cover(NULL);
}

void screensaver_hide(void) {
	if (!saver || !screensaver_is_visible()) {
		return;
	}
	slide_to(-screen_h);
}

// The swipe up, following the finger the whole way: the screensaver slides
// with it, and the release decides whether it carries on off the top or falls
// back into place. Same feel as the control centre.
static void drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		lv_anim_delete(saver, anim_y_cb);
		tracking = true;
		engaged = false;
		lock_dragged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dy = point.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (dy > -DRAG_ENGAGE_PX) {
				return; // not (yet) an upward drag
			}
			engaged = true;
			lock_dragged = true;
		}
		// Only upward: dragging down does not pull it off the bottom.
		lv_obj_set_y(saver, dy < 0 ? dy : 0);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		// A quarter of the screen of travel and it goes; short of that it
		// falls back.
		slide_to(dy < -screen_h / 4 ? -screen_h : 0);
	}
}

void screensaver_init(gui_config_t *cfg) {
	screen_w = cfg->screen_width;
	screen_h = cfg->screen_height;
	strip_h = DEFAULT_STRIP_H;

	screensaver_set_card_root(cfg->sd_root_path);
	enabled = config_get_bool("screen", "screensaver", false);
	source = config_get_int("screen", "screensaver_source", SCREENSAVER_SOURCE_ALBUM) == SCREENSAVER_SOURCE_IMAGES
				 ? SCREENSAVER_SOURCE_IMAGES
				 : SCREENSAVER_SOURCE_ALBUM;
	cover_box_h = screen_h - strip_h;

	saver = lv_obj_create(lv_layer_top());
	lv_obj_set_size(saver, screen_w, screen_h);
	lv_obj_set_pos(saver, 0, 0);
	lv_obj_set_style_bg_color(saver, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(saver, 0, 0);
	lv_obj_set_style_radius(saver, 0, 0);
	lv_obj_set_style_pad_all(saver, 0, 0);
	lv_obj_set_scrollable(saver, false);
	lv_obj_set_clickable(saver, true);
	lv_obj_set_hidden(saver, true);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// The artwork: everything above the strip, centre-cropped to fill it.
	cover_box = lv_obj_create(saver);
	lv_obj_set_size(cover_box, screen_w, screen_h - strip_h);
	lv_obj_set_pos(cover_box, 0, 0);
	lv_obj_set_style_bg_color(cover_box, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cover_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cover_box, 0, 0);
	lv_obj_set_style_radius(cover_box, 0, 0);
	lv_obj_set_style_pad_all(cover_box, 0, 0);
	lv_obj_set_style_clip_corner(cover_box, true, 0);
	lv_obj_set_scrollable(cover_box, false);
	lv_obj_set_event_bubble(cover_box, true);
	lv_obj_set_clickable(cover_box, false);

	// Nothing to show for a track with no artwork: the same note the player
	// puts in the same place, so the screensaver is recognisably the player
	// with the controls taken away rather than a different screen that has
	// gone wrong.
	cover_placeholder = lv_image_create(cover_box);
	lv_image_set_src(cover_placeholder, &icon_music_note);
	lv_obj_set_style_image_recolor(cover_placeholder, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(cover_placeholder, LV_OPA_COVER, 0);
	lv_obj_center(cover_placeholder);

	cover_img = lv_image_create(cover_box);
	lv_obj_center(cover_img);
	lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_CENTER);

	// The blurred block: the same picture the player draws its controls on.
	info_strip = lv_obj_create(saver);
	lv_obj_set_size(info_strip, screen_w, strip_h);
	lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(info_strip, 0, 0);
	lv_obj_set_style_radius(info_strip, 0, 0);
	lv_obj_set_style_pad_hor(info_strip, cfg->padding + 6, 0);
	lv_obj_set_style_pad_ver(info_strip, 18, 0);
	lv_obj_set_style_pad_gap(info_strip, 2, 0);
	lv_obj_set_scrollable(info_strip, false);
	lv_obj_set_event_bubble(info_strip, true);
	lv_obj_set_clickable(info_strip, false);
	lv_obj_set_flex_flow(info_strip, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(info_strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The blurred backdrop is always the dark treatment, so the text on it is
	// light in both themes.
	title_label = lv_label_create(info_strip);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_set_style_text_font(title_label, &font_ui_28, 0);
	lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(title_label);

	artist_label = lv_label_create(info_strip);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_set_style_text_font(artist_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(artist_label, lv_color_make(200, 200, 200), 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(artist_label);

	// The time, in the largest figures the screen has.
	clock_label = clocklabel_create(info_strip, &font_ui_72, 72, &font_ui_32, 32);
	lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
	lv_obj_set_style_pad_top(clock_label, 6, 0);

	// The way out, spelled out: swipe up. A sign, not a button -- tapping it
	// does nothing, since the screensaver is only dismissed by dragging -- so it
	// is not clickable and a finger landing on it talks to the saver underneath,
	// where the drag gesture lives.
	lock_build();

	lv_obj_t *chevron_btn = lv_obj_create(saver);
	lv_obj_set_size(chevron_btn, 80, 44);
	lv_obj_align(chevron_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
	lv_obj_set_style_bg_opa(chevron_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(chevron_btn, 0, 0);
	lv_obj_set_style_shadow_width(chevron_btn, 0, 0);
	lv_obj_set_style_pad_all(chevron_btn, 0, 0);
	lv_obj_set_clickable(chevron_btn, false);
	lv_obj_set_scrollable(chevron_btn, false);

	lv_obj_t *chevron = lv_image_create(chevron_btn);
	lv_image_set_src(chevron, &icon_chevron_up);
	lv_obj_set_style_image_recolor(chevron, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(chevron, LV_OPA_70, 0);
	lv_obj_center(chevron);

	// One second, not ten: the clock only needs a minute's resolution, but the
	// same tick is what notices a track change, and a screensaver that shows
	// the previous song for ten seconds is a screensaver that looks broken.
	clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
	lv_timer_pause(clock_timer);
	// It is drawn into the framebuffer before the blank so the next wake opens
	// on it -- but with the panel off nothing is scanned out, so ticking it
	// once a second all night buys nothing.
	power_pause_in_standby(clock_timer);
}
