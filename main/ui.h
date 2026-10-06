#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

/* Boot menu selection */
typedef enum
{
    BOOT_OPT_PLAYER = 0,
    BOOT_OPT_UPLOAD,
} boot_option_t;

/* Initialize the UI elements */
void ui_init(void);

/* Update functions */
void ui_set_status(const char *text);

void ui_notify_track_started(const char *path,
                             const void *cover_src,
                             int index,
                             int count,
                             uint32_t duration_sec);

void ui_notify_track_finished(const char *path);

void ui_show_volume_mode(bool show);
void ui_update_volume(uint8_t volume);
void ui_update_mute(bool mute);
void ui_update_playback(bool playing);
void ui_update_elapsed(uint32_t elapsed_sec);

void ui_show_boot_menu(bool show);
void ui_set_boot_selection(boot_option_t selection);

void ui_show_transfer_screen(bool show,
                             const char *ssid,
                             const char *password,
                             const char *url);

void ui_set_transfer_status(const char *text);
void ui_update_transfer_progress(uint32_t done_bytes, uint32_t total_bytes);

#endif // UI_H