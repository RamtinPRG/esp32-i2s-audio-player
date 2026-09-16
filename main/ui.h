#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

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

/* New UX functions for Two-Mode State Machine */
void ui_show_volume_mode(bool show);
void ui_update_volume(uint8_t volume);
void ui_update_mute(bool mute);
void ui_update_playback(bool playing);

#endif // UI_H