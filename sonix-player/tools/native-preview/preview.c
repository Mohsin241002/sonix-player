#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "src/gui/audio/dacpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/library/filespage.h"
#include "src/gui/library/music.h"
#include "src/gui/settings/appearance.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

static uint16_t *frame;
static int flush_count;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;

extern int preview_player_opens;
extern int preview_play_pause_calls;
extern int preview_next_calls;
extern int preview_prev_calls;
extern int preview_ebookpage_opens;
extern int preview_filespage_opens;

static void capture(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    int width = lv_display_get_horizontal_resolution(display);
    size_t row_bytes = (size_t)(area->x2 - area->x1 + 1) * sizeof(*frame);
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(frame + width * y + area->x1, pixels + (y - area->y1) * row_bytes, row_bytes);
    }
    flush_count++;
    lv_display_flush_ready(display);
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    data->point = pointer_point;
    data->state = pointer_state;
    data->continue_reading = false;
}

static void pump(unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        lv_tick_inc(30);
        lv_timer_handler();
    }
}

static void tap_at(int x, int y) {
    pointer_point.x = x;
    pointer_point.y = y;
    pointer_state = LV_INDEV_STATE_PRESSED;
    pump(3);
    pointer_state = LV_INDEV_STATE_RELEASED;
    pump(4);
}

static lv_obj_t *find_label(lv_obj_t *root, const char *text) {
    if (!root) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0) return root;
    uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_image(lv_obj_t *root, const lv_image_dsc_t *source) {
    if (!root) return NULL;
    if (lv_obj_check_type(root, &lv_image_class) && lv_image_get_src(root) == source) return root;
    uint32_t count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *found = find_image(lv_obj_get_child(root, i), source);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *click_target(lv_obj_t *object) {
    for (lv_obj_t *p = object; p; p = lv_obj_get_parent(p)) {
        if (lv_obj_is_clickable(p)) return p;
    }
    return NULL;
}

static bool tap_object(lv_obj_t *object) {
    lv_obj_t *target = click_target(object);
    if (!target) return false;
    lv_obj_update_layout(lv_screen_active());
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    tap_at((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    return true;
}

static bool tap_label(lv_obj_t *root, const char *tag) {
    lv_obj_t *text = find_label(root, tr(tag));
    return text && tap_object(text);
}

static bool back_to(lv_obj_t *screen) {
    lv_obj_t *chevron = find_image(lv_layer_top(), &icon_chevron_left);
    return chevron && tap_object(chevron) && lv_screen_active() == screen;
}

static bool run_input_tests(bool playing) {
    switch_screen(main_menu_screen);
    pump(8);
    if (!tap_label(main_menu_screen, "home_apps") || lv_screen_active() != main_menu_apps_screen) {
        fprintf(stderr, "Input test: dock Apps did not open the app drawer\n");
        return false;
    }

    struct app_case { const char *tag; lv_obj_t *target; int *action_calls; } cases[] = {
        {"music", music_screen, NULL},
        {"books", ebookpage_screen, &preview_ebookpage_opens},
        {"audiobooks", audiobooks_screen, NULL},
        {"streaming", streaming_screen, NULL},
        {"wireless", wireless_screen, NULL},
        {"file_explorer", filespage_screen, &preview_filespage_opens},
        {"dac", dacpage_screen, NULL},
        {"more", morepage_screen, NULL},
        {"settings", settings_screen, NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before_action = cases[i].action_calls ? *cases[i].action_calls : 0;
        if (!tap_label(main_menu_apps_screen, cases[i].tag)) {
            fprintf(stderr, "Input test: drawer entry '%s' was not tappable\n", cases[i].tag);
            return false;
        }
        pump(8);
        if (lv_screen_active() != cases[i].target ||
            (cases[i].action_calls && *cases[i].action_calls != before_action + 1)) {
            fprintf(stderr, "Input test: drawer entry '%s' reached the wrong destination\n", cases[i].tag);
            return false;
        }
        if (!back_to(main_menu_apps_screen)) {
            fprintf(stderr, "Input test: Back from '%s' did not return to the drawer\n", cases[i].tag);
            return false;
        }
        pump(4);
    }
    if (!back_to(main_menu_screen)) {
        fprintf(stderr, "Input test: Back from the drawer did not return home\n");
        return false;
    }

    if (playing) {
        const lv_image_dsc_t *buttons[] = {&icon_skip_back_large, &icon_pause_large, &icon_skip_forward_large};
        int before_open = preview_player_opens;
        int before_prev = preview_prev_calls;
        int before_toggle = preview_play_pause_calls;
        int before_next = preview_next_calls;
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            lv_obj_t *image = find_image(main_menu_screen, buttons[i]);
            if (!image || !tap_object(image)) {
                fprintf(stderr, "Input test: home player control %zu was not tappable\n", i);
                return false;
            }
            pump(4);
        }
        if (preview_player_opens != before_open || preview_prev_calls != before_prev + 1 ||
            preview_play_pause_calls != before_toggle + 1 || preview_next_calls != before_next + 1) {
            fprintf(stderr, "Input test: home player controls opened the sheet or missed an action\n");
            return false;
        }
    }

    bool was_large = fonts_large_text();
    if (!fonts_set_large_text(!was_large)) {
        fprintf(stderr, "Input test: failed to switch text size\n");
        return false;
    }
    lv_obj_update_layout(main_menu_screen);
    if (!find_label(main_menu_screen, "Sonix") || !fonts_set_large_text(was_large)) {
        fprintf(stderr, "Input test: text-size change damaged home labels\n");
        return false;
    }
    lv_obj_update_layout(main_menu_screen);
    return true;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "Usage: %s home|apps|music|appearance 720|800 dark|light normal|large LANGUAGE OUTPUT.rgb565\n", argv[0]);
        return 1;
    }
    int width = 480;
    int height = atoi(argv[2]);
    if (height != 720 && height != 800) {
        return 1;
    }
    bool dark = strcmp(argv[3], "dark") == 0;
    bool large = strcmp(argv[4], "large") == 0;
    gui_config_t cfg = {
        .screen_width = width,
        .screen_height = height,
        .padding = 15,
        .top_bar_height = 44,
        .sd_root_path = ".",
    };

    config_set_bool("ui", "dark_theme", dark);
    config_set_int("ui", "text_size", large ? FONTS_TEXT_LARGE : FONTS_TEXT_NORMAL);
    config_set("ui", "language", argv[5]);

    lv_init();
    lang_init("../sonix-packer/assets/R1/usr/resource/sonix/language");
    if (!fonts_init()) {
        return 1;
    }
    size_t pixel_count = (size_t)width * height;
    frame = calloc(pixel_count, sizeof(*frame));
    uint16_t *render_buffer = calloc(pixel_count, sizeof(*render_buffer));
    if (!frame || !render_buffer) {
        return 1;
    }
    lv_display_t *display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, render_buffer, NULL, pixel_count * sizeof(*frame), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, capture);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);

    theme_init();
    main_menu_screen = lv_obj_create(NULL);
    music_screen = lv_obj_create(NULL);
    appearance_screen = lv_obj_create(NULL);
    settings_screen = lv_obj_create(NULL);
    streaming_screen = lv_obj_create(NULL);
    wireless_screen = lv_obj_create(NULL);
    audiobooks_screen = lv_obj_create(NULL);
    morepage_screen = lv_obj_create(NULL);
    ebookpage_screen = lv_obj_create(NULL);
    dacpage_screen = lv_obj_create(NULL);
    filespage_screen = lv_obj_create(NULL);

    topbar_init(&cfg);
    back_btn_init(&cfg);
    main_menu_init(&cfg);
    music_init(&cfg);
    appearance_init(&cfg);

    lv_obj_t *target = main_menu_screen;
    if (strcmp(argv[1], "music") == 0) {
        target = music_screen;
    } else if (strcmp(argv[1], "appearance") == 0) {
        target = appearance_screen;
    } else if (strcmp(argv[1], "apps") == 0) {
        target = main_menu_apps_screen;
    } else if (strcmp(argv[1], "home") != 0) {
        return 1;
    }
    switch_screen(main_menu_screen);
    switch_screen(target);
    pump(10);
    if (getenv("SONIX_PREVIEW_INPUT_TEST") && !run_input_tests(getenv("SONIX_PREVIEW_PLAYING") != NULL)) {
        return 1;
    }
    switch_screen(target);
    pump(10);
    lv_refr_now(display);
    if (flush_count == 0) {
        fprintf(stderr, "LVGL did not render any pixels\n");
        return 1;
    }

    FILE *output = fopen(argv[6], "wb");
    if (!output) {
        perror(argv[6]);
        return 1;
    }
    size_t written = fwrite(frame, sizeof(*frame), pixel_count, output);
    int close_status = fclose(output);
    return written != pixel_count || close_status != 0;
}
