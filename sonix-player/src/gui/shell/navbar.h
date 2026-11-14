#ifndef NAVBAR_H
#define NAVBAR_H

#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"
#include "src/system/playback/device_state.h"

// The two bars along the foot of the main pages.
//
// The tab bar is the way between the five places the player is organised
// around -- Home, Library, Search, Books and More -- so none of them is more
// than one tap from any other. The mini-player sits just above it on every
// page but Home (which carries a large player of its own) and keeps the track
// and the transport in reach once the player sheet is closed.
//
// Both are ordinary children of the page they are attached to, built once with
// it. Nothing runs while the page is hidden: the mini-player is repainted from
// the player's existing progress update, and only on the page on screen.

typedef enum {
	NAVBAR_NONE = -1,
	NAVBAR_HOME,
	NAVBAR_LIBRARY,
	NAVBAR_SEARCH,
	NAVBAR_BOOKS,
	NAVBAR_MORE,
} navbar_tab_t;

// Height of each bar, in pixels.
#define NAVBAR_TABS_HEIGHT 80
#define NAVBAR_MINI_HEIGHT 76

// Puts the tab bar at the foot of `screen`, with `active` lit, and the
// mini-player above it when `mini` is set. Returns how much of the bottom of
// the screen the two take, for the page to keep its own content clear of.
int navbar_attach(lv_obj_t *screen, gui_config_t *cfg, navbar_tab_t active, bool mini);

// Only the mini-player, flush with the bottom of `screen`: for the pages one
// level down, which have a back button and no tab bar. Returns its height.
int navbar_attach_mini(lv_obj_t *screen, gui_config_t *cfg);

// Detail pages share a single mini-player. Reserve the returned height in
// the scroll viewport. Create full-screen dialogs on the top layer.
int navbar_use_shared_mini(lv_obj_t *screen, gui_config_t *cfg);

// Repaints the mini-player on the page on screen, if it has one. Called from
// the same update that refreshes the home player.
// The mini-player's 52 px picture of the playing track, NULL when it has none.
const lv_image_dsc_t *navbar_thumb(void);

void navbar_refresh_player(const device_state_t *state);

#endif // NAVBAR_H
