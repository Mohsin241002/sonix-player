#ifndef MAIN_MENU_H
#define MAIN_MENU_H

#include "src/gui/shell/gui.h"
#include "src/system/playback/device_state.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *main_menu_screen;
extern lv_obj_t *main_menu_apps_screen;

void main_menu_init(gui_config_t *cfg);
void main_menu_refresh_player(const device_state_t *state);

#endif
