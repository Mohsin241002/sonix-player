#include "popover.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#define POPOVER_MAX_ITEMS 7
#define POPOVER_ROW_HEIGHT 76
#define POPOVER_RADIUS 24
// The sheet runs this far past the bottom edge, so only its top corners show
// rounded: LVGL rounds all four or none.
#define SHEET_TUCK 28

// The glyph a row wears, for the actions that have an obvious one. Looked up
// by the row's tag, so the thirty menus that use this file get them without
// each naming a picture.
static const lv_image_dsc_t *item_glyph(const char *label) {
	static const struct {
		const char *label;
		const lv_image_dsc_t *glyph;
	} GLYPHS[] = {
		{"trackmenu_queue", &icon_list_queue},
		{"add_to_queue", &icon_list_plus},
		{"add_to_playlist", &icon_plus},
		{"medialist_remove_from_playlist", &icon_list_x},
		{"show_album", &icon_menu_album},
		{"trackmenu_show_podcast", &icon_menu_album},
		{"details", &icon_info},
		{"settings", &icon_menu_settings},
		{"medialist_add_to_favourites", &icon_star},
		{"remove_from_favourites", &icon_star_x},
		{"podcast_play_next", &icon_play_next},
		{"rename", &icon_rename},
		{"files_move_to", &icon_wifi_transfer_quick},
		{"files_copy_to", &icon_copy},
		{"delete", &icon_trash},
		{"bt_forget", &icon_trash},
		{"wifi_forget_network", &icon_trash},
		{"ebookmarks_delete_bookmark", &icon_trash},
		{"podcast_delete_from_card_short", &icon_trash},
		{"chapters", &icon_chapter},
		{"bookmarks", &icon_bookmark},
		{"audiobook_add_bookmark", &icon_bookmark},
		{"audiobook_summary", &icon_info},
		{"connect", &icon_bluetooth},
		{"disconnect", &icon_close},
		{"playlist_backup", &icon_download},
	};
	for (size_t i = 0; label && i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); i++) {
		if (strcmp(label, GLYPHS[i].label) == 0) {
			return GLYPHS[i].glyph;
		}
	}
	return NULL;
}

static lv_obj_t *veil; // full-screen, catches the tap that dismisses
static lv_obj_t *card;

static popover_item_t items_copy[POPOVER_MAX_ITEMS];
static int items_count;

static const void *head_cover;
static char head_title[256];
static char head_sub[256];
static bool head_pending;

void popover_set_header(const void *cover_src, const char *title, const char *subtitle) {
	head_cover = cover_src;
	snprintf(head_title, sizeof(head_title), "%s", title ? title : "");
	snprintf(head_sub, sizeof(head_sub), "%s", subtitle ? subtitle : "");
	head_pending = title && title[0];
}

static void head_close_cb(lv_event_t *e) {
	(void)e;
	popover_close();
}

static void build_header(void) {
	lv_obj_t *row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), 88);
	lv_obj_set_style_pad_left(row, 20, 0);
	lv_obj_set_style_pad_right(row, 4, 0);
	lv_obj_set_style_pad_column(row, 16, 0);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(row, 1, 0);
	lv_obj_set_style_border_color(row, theme()->surface_pressed, 0);
	lv_obj_set_style_margin_bottom(row, 4, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);

	if (head_cover) {
		lv_obj_t *cover = lv_image_create(row);
		lv_image_set_src(cover, head_cover);
		lv_obj_set_size(cover, 60, 60);
		lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_STRETCH);
		lv_obj_set_style_radius(cover, 8, 0);
		lv_obj_set_style_clip_corner(cover, true, 0);
	}

	lv_obj_t *text = lv_obj_create(row);
	lv_obj_remove_style_all(text);
	lv_obj_set_height(text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(text, 2, 0);
	lv_obj_t *title = lv_label_create(text);
	lv_label_set_text(title, head_title);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_24_bold, 0);
	if (head_sub[0]) {
		lv_obj_t *sub = lv_label_create(text);
		lv_label_set_text(sub, head_sub);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_20, 0);
	}

	lv_obj_t *close = lv_btn_create(row);
	lv_obj_set_size(close, 64, 64);
	lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(close, 0, 0);
	lv_obj_set_style_border_width(close, 0, 0);
	lv_obj_add_event_cb(close, head_close_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *x = lv_image_create(close);
	lv_image_set_src(x, &icon_close);
	lv_obj_set_style_image_recolor(x, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(x, LV_OPA_COVER, 0);
	lv_obj_center(x);
}

void popover_close(void) {
	head_pending = false;
	if (veil) {
		lv_obj_set_hidden(veil, true);
	}
}

static void veil_clicked_cb(lv_event_t *e) {
	(void)e;
	popover_close();
}

static void item_clicked_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= items_count) {
		return;
	}

	// Close first: the action may well open another page, and the veil must
	// not linger over it.
	popover_item_t item = items_copy[index];
	popover_close();

	if (item.action) {
		item.action(item.user);
	}
}

void popover_init(void) {
	if (veil) {
		return;
	}

	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0); // heavy dim: the menu owns the screen
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_hidden(veil, true);
	lv_obj_add_event_cb(veil, veil_clicked_cb, LV_EVENT_CLICKED, NULL);

	card = lv_obj_create(veil);
	// A sheet from the bottom edge, the design's shape for every menu: the
	// full width, a grab bar, 76 px rows.
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, POPOVER_RADIUS, 0);
	lv_obj_set_style_border_width(card, 2, 0);
	lv_obj_set_style_border_side(card, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_color(card, theme()->surface_pressed, 0);
	lv_obj_set_style_pad_all(card, 0, 0);
	lv_obj_set_style_pad_top(card, 6, 0);
	lv_obj_set_style_pad_bottom(card, 12 + SHEET_TUCK, 0);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(card, false);
	// A tap on the card itself must not fall through to the veil.
	lv_obj_set_event_bubble(card, false);
}

void popover_show(lv_obj_t *anchor, const popover_item_t *items, int count) {
	if (!veil || !items || count <= 0) {
		return;
	}
	if (count > POPOVER_MAX_ITEMS) {
		count = POPOVER_MAX_ITEMS;
	}

	memcpy(items_copy, items, (size_t)count * sizeof(items[0]));
	items_count = count;

	// Rebuild the rows.
	lv_obj_clean(card);
	lv_obj_t *grab = lv_obj_create(card);
	lv_obj_remove_style_all(grab);
	lv_obj_set_size(grab, 56, 6);
	lv_obj_set_style_radius(grab, 3, 0);
	lv_obj_set_style_bg_color(grab, theme()->text_secondary, 0);
	lv_obj_set_style_bg_opa(grab, LV_OPA_70, 0);
	lv_obj_set_style_margin_top(grab, 8, 0);
	lv_obj_set_style_margin_bottom(grab, 10, 0);
	lv_obj_set_clickable(grab, false);
	if (head_pending) {
		build_header();
		head_pending = false;
	}
	// One column for the glyphs when any row has one, so the words line up.
	bool glyphs = false;
	for (int i = 0; i < count; i++) {
		glyphs = glyphs || item_glyph(items[i].label) != NULL;
	}
	for (int i = 0; i < count; i++) {
		lv_obj_t *row = lv_btn_create(card);
		// Tall enough for one line, taller when the text needs two: a
		// translated label such as "Zur Wiedergabeliste hinzufügen" does not
		// fit on one, and a fixed height would clip it at the card's edge.
		lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_style_min_height(row, POPOVER_ROW_HEIGHT, 0);
		lv_obj_set_style_pad_ver(row, 12, 0);
		// With a check mark the entry lays out as a row: text left, mark right.
		// Without one it stays a column, which is what the two-line text of long
		// entries needs.
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, 16, 0);
		if (glyphs) {
			lv_obj_t *glyph = lv_image_create(row);
			const lv_image_dsc_t *src = item_glyph(items[i].label);
			lv_obj_set_size(glyph, 30, 30);
			lv_image_set_inner_align(glyph, LV_IMAGE_ALIGN_CONTAIN);
			if (src) {
				lv_image_set_src(glyph, src);
				lv_obj_add_style(glyph, &theme_style_icon, 0);
				lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
			}
		}
		lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row, 0, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 20, 0);
		lv_obj_add_event_cb(row, item_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(row);
		lv_label_set_text(label, tr(items[i].label));
		lv_obj_add_style(label, &theme_style_text, 0);
		lv_obj_set_style_text_font(label, &font_ui_24, 0);
		// Given a width, a label wraps instead of running past its parent. Left
		// aligned on both lines: a menu is read down its left edge, and centred
		// second lines would make that edge ragged.
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
		lv_obj_set_flex_grow(label, 1);
		lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
		if (items[i].checked) {

			lv_obj_t *check = lv_image_create(row);
			lv_image_set_src(check, &icon_check);
			lv_obj_set_style_image_recolor(check, theme()->accent, 0);
			lv_obj_set_style_image_recolor_opa(check, LV_OPA_COVER, 0);
		}

		// A hairline between rows, not after the last.
		if (0) {
			lv_obj_t *sep = lv_obj_create(card);
			lv_obj_set_size(sep, lv_pct(94), 1);
			lv_obj_set_style_bg_color(sep, theme()->text_secondary, 0);
			lv_obj_set_style_bg_opa(sep, LV_OPA_20, 0);
			lv_obj_set_style_border_width(sep, 0, 0);
		}
	}

	// Against the bottom edge, whatever opened it.
	(void)anchor;
	lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, SHEET_TUCK);

	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);
}
