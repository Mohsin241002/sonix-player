/* External fixtures for services the rendered pages call on the device. */
#include <stdio.h>
#include <stdlib.h>

#include "src/gui/library/audiobooks.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/filespage.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/audio/dacpage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/settings/bootlogopage.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/animpark.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/audio/usbdac.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/clock.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/device/usb.h"
#include "src/system/library/library.h"
#include "src/system/net/wifi.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/streaming/podcastcache.h"

lv_obj_t *audiobooks_screen;
lv_obj_t *audiobookscan_screen;
lv_obj_t *bootlogopage_screen;
lv_obj_t *browser_screen;
lv_obj_t *libraryscan_screen;
lv_obj_t *ebookpage_screen;
lv_obj_t *dacpage_screen;
lv_obj_t *filespage_screen;
lv_obj_t *morepage_screen;
lv_obj_t *musicsettings_screen;
lv_obj_t *player_screen;
lv_obj_t *settings_screen;
lv_obj_t *streaming_screen;
lv_obj_t *wireless_screen;

void device_state_get(device_state_t *out) {
    *out = (device_state_t){.volume = 36};
    if (getenv("SONIX_PREVIEW_PLAYING")) {
        out->status = AUDIO_STATUS_PLAYING;
        snprintf(out->current_file, sizeof(out->current_file), "/music/preview-track.flac");
        snprintf(out->metadata.title, sizeof(out->metadata.title), "A quiet morning");
        snprintf(out->metadata.artist, sizeof(out->metadata.artist), "Demo track");
        out->progress_current_secs = 72;
        out->progress_total_secs = 224;
    }
    snprintf(out->battery_percent, sizeof(out->battery_percent), "82");
}

void wifi_get_status(wifi_status_t *out) {
    *out = (wifi_status_t){.state = WIFI_STATE_CONNECTED, .bars = 3};
}

bool wifi_get_enabled(void) { return true; }
bool gui_card_available(void) { return true; }
int library_track_count(void) { return 42; }
int headphone_jack_state(void) { return JACK_NONE; }
bool clock_is_set(void) { return true; }
void clock_format_hm_parts(char *time_out, size_t time_size, char *suffix_out, size_t suffix_size, int hour, int minute) {
    (void)hour; (void)minute;
    snprintf(time_out, time_size, "9:41");
    snprintf(suffix_out, suffix_size, "AM");
}

int preview_player_opens;
void player_sheet_open(bool animate) { (void)animate; preview_player_opens++; }
int preview_play_pause_calls;
int preview_next_calls;
int preview_prev_calls;
int preview_ebookpage_opens;
int preview_filespage_opens;
const lv_image_dsc_t *player_cover_image(void) { return NULL; }
uint64_t player_cover_identity(void) { return 0; }
void ebookpage_open(void) {
    preview_ebookpage_opens++;
    switch_screen(ebookpage_screen);
}
void filespage_open(void) {
    preview_filespage_opens++;
    switch_screen(filespage_screen);
}

void airplay_get_state(airplay_state_t *out) { *out = (airplay_state_t){0}; }
void animpark_wake(lv_obj_t *root) { (void)root; }
int audio_get_dsd_multiple(void) { return 0; }
void audio_get_output_device(char *out, size_t size) { if (size) out[0] = 0; }
bool bluetooth_audio_active(void) { return false; }
int bluetooth_get_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size) {
    (void)out; (void)max;
    if (selected_size) selected[0] = 0;
    return 0;
}
bool bluetooth_get_enabled(void) { return false; }
bt_state_t bluetooth_get_state(void) { return BT_STATE_OFF; }
bool bluetooth_receiver_stream(bt_stream_t *out) { (void)out; return false; }
void bootlogo_follow(bool dark, int accent) { (void)dark; (void)accent; }
bool browser_can_go_up(void) { return false; }
bool browser_go_up(void) { return false; }
void browser_refresh(void) {}
bool btreceiver_is_active(void) { return false; }
bool clock_poll(void) { return false; }
void clock_remember(void) {}
bool clock_resync(void) { return false; }
void clock_set_change_hook(void (*hook)(void)) { (void)hook; }
void coverflow_attach_edge(lv_obj_t *obj, gui_config_t *cfg) { (void)obj; (void)cfg; }
bool coverflow_drag_active(void) { return false; }
void device_state_refresh_battery(void) {}
void gui_notify_popup(const char *text) { fprintf(stderr, "Preview popup: %s\n", text); }
bool gui_post(void (*cb)(void *), void *user) { cb(user); return true; }
void led_set_bt_receiver(bool on) { (void)on; }
void led_set_charging(bool on) { (void)on; }
void led_set_dac(bool active, bool streaming, int sample_rate) {
    (void)active; (void)streaming; (void)sample_rate;
}
void led_set_on_charger(bool on) { (void)on; }
void led_set_wifi_transfer(bool on) { (void)on; }
void led_update_playback(bool playing, int sample_rate, bool podcast, bool dsd) {
    (void)playing; (void)sample_rate; (void)podcast; (void)dsd;
}
bool library_scan_running(void) { return false; }
void libraryscan_begin(void) {}
void libraryscan_choose_folders(void) {}
void lineout_check_jack(void) {}
void medialist_open(const char *title, library_list_t kind, library_filter_t filter, const char *filter_value) {
    (void)title; (void)kind; (void)filter; (void)filter_value;
}
void player_output_unplugged(const char *what) { (void)what; }
void player_set_statusbar_always(bool on) { (void)on; }
void player_sheet_attach_drag(lv_obj_t *obj, bool opening) { (void)obj; (void)opening; }
void player_sheet_close(bool animate) { (void)animate; }
bool player_sheet_drag_active(void) { return false; }
bool player_sheet_is_open(void) { return false; }
bool player_statusbar_always(void) { return false; }
void playlistpage_open(void) {}
bool podcastcache_is_episode(const char *path) { (void)path; return false; }
bool power_charging_held(void) { return false; }
void power_pause_in_standby(lv_timer_t *timer) { (void)timer; }
void power_slow_in_standby(lv_timer_t *timer, uint32_t period) { (void)timer; (void)period; }
bool quickpanel_blocked(void) { return false; }
void quickpanel_drag_begin(void) {}
void quickpanel_drag_end(void) {}
void quickpanel_drag_update(int dy) { (void)dy; }
bool quickpanel_is_open(void) { return false; }
void quickpanel_open(void) {}
void search_open(void) {}
bool sonixlink_is_connected(void) { return false; }
bool usb_storage_active(void) { return false; }
bool usb_vbus_present(void) { return false; }
bool usbaudio_active(void) { return false; }
void usbaudio_poll(void) {}
void usbdac_get_state(usbdac_state_t *out) { *out = (usbdac_state_t){0}; }
void volume_profile_persist(void) {}
bool wifitransfer_get_enabled(void) { return false; }

void player_key_play_pause(void) { preview_play_pause_calls++; }
void player_key_next(void) { preview_next_calls++; }
void player_key_prev(void) { preview_prev_calls++; }
