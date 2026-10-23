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

/* Call once after display/UI initialization, before starting playback. */
esp_err_t ui_presenter_init(void);

/* Task-context only. Copies the snapshot without waiting for LVGL. */
esp_err_t ui_presenter_publish_playback(const playback_view_t *view);

#endif