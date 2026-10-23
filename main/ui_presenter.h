#ifndef UI_PRESENTER_H
#define UI_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct
{
    bool track_valid;
    uint32_t track_revision;

    char path[MAX_PATH_LEN];

    /*
     * Borrowed artwork pointer.
     * The descriptor and pixel storage must remain alive while
     * the UI displays or animates this artwork.
     */
    const void *cover_src;

    int track_index;
    int track_count;

    uint32_t duration_sec;
    uint32_t elapsed_sec;
    bool playing;
} playback_view_t;

typedef struct
{
    bool upload_selected;
    bool volume_overlay_visible;
    uint8_t volume;
    bool muted;
} controls_view_t;

typedef enum
{
    UI_SCREEN_BOOT = 0,
    UI_SCREEN_PLAYER,
    UI_SCREEN_TRANSFER,
} ui_screen_t;

/* Call once after display/UI initialization, before starting playback. */
esp_err_t ui_presenter_init(void);

/* Task-context only. Copies the snapshot without waiting for LVGL. */
esp_err_t ui_presenter_publish_playback(const playback_view_t *view);

/* Task-context only. Copies a complete controls snapshot. */
esp_err_t ui_presenter_publish_controls(const controls_view_t *view);

/*
 * Task-context only.
 * status is copied and may be NULL.
 * Status text applies to the player screen's track/status label.
 */
esp_err_t ui_presenter_show_screen(
    ui_screen_t screen,
    const char *status);

/* Task-context only. Text is copied into presenter-owned storage. */
void ui_presenter_set_transfer_status(const char *status);

void ui_presenter_update_transfer_progress(uint32_t done_bytes, uint32_t total_bytes);

/* Update status and progress together. */
void ui_presenter_set_transfer_state(const char *status, uint32_t done_bytes, uint32_t total_bytes);

#endif