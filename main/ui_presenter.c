#include "ui_presenter.h"

#include "ui.h"

#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include "freertos/task.h"
#include "app_types.h"

#define SCREEN_STATUS_MAX_LEN 96
#define TRANSFER_STATUS_MAX_LEN 96

typedef struct
{
    char status[TRANSFER_STATUS_MAX_LEN];
    uint32_t done_bytes;
    uint32_t total_bytes;

    bool status_dirty;
    bool progress_dirty;
} transfer_view_t;

static portMUX_TYPE s_transfer_lock = portMUX_INITIALIZER_UNLOCKED;
static transfer_view_t s_transfer_view;

typedef struct
{
    ui_screen_t screen;
    bool has_status;
    char status[SCREEN_STATUS_MAX_LEN];
} screen_view_t;

static QueueHandle_t s_screen_queue = NULL;

/* Accessed only from the LVGL timer callback. */
static bool s_has_rendered_screen = false;
static ui_screen_t s_rendered_screen;

static QueueHandle_t s_playback_queue = NULL;
static lv_timer_t *s_presenter_timer = NULL;

/* Accessed only from the LVGL timer callback. */
static bool s_has_rendered_track = false;
static uint32_t s_rendered_track_revision = 0;
static uint32_t s_rendered_elapsed_sec = 0;
static bool s_rendered_playing = false;

static QueueHandle_t s_controls_queue = NULL;

/* Accessed only from the LVGL timer callback. */
static bool s_has_rendered_controls = false;
static controls_view_t s_rendered_controls;

static void present_controls(void)
{
    controls_view_t view;

    if (xQueueReceive(s_controls_queue, &view, 0) != pdTRUE)
    {
        return;
    }

    bool first = !s_has_rendered_controls;

    if (first ||
        view.upload_selected != s_rendered_controls.upload_selected)
    {
        ui_set_boot_selection(
            view.upload_selected ? BOOT_OPT_UPLOAD : BOOT_OPT_PLAYER);
    }

    /*
     * Update these together, in this order.
     * ui_update_volume restores the normal label and icon;
     * ui_update_mute then applies the muted appearance if needed.
     */
    if (first ||
        view.volume != s_rendered_controls.volume ||
        view.muted != s_rendered_controls.muted)
    {
        ui_update_volume(view.volume);
        ui_update_mute(view.muted);
    }

    if (first ||
        view.volume_overlay_visible !=
            s_rendered_controls.volume_overlay_visible)
    {
        ui_show_volume_mode(view.volume_overlay_visible);
    }

    s_rendered_controls = view;
    s_has_rendered_controls = true;
}

static void present_screen(void)
{
    screen_view_t view;

    if (xQueueReceive(s_screen_queue, &view, 0) != pdTRUE)
    {
        return;
    }

    bool screen_changed =
        !s_has_rendered_screen ||
        view.screen != s_rendered_screen;

    if (screen_changed)
    {
        /*
         * Hide the old full-screen overlays before showing the
         * requested screen. The player is the underlying screen.
         */
        ui_show_boot_menu(false);
        ui_show_transfer_screen(false, NULL, NULL, NULL);

        switch (view.screen)
        {
        case UI_SCREEN_BOOT:
            ui_show_boot_menu(true);
            break;

        case UI_SCREEN_PLAYER:
            break;

        case UI_SCREEN_TRANSFER:
            ui_show_transfer_screen(
                true,
                WIFI_AP_SSID,
                WIFI_AP_PASSWORD,
                WIFI_TRANSFER_URL);
            break;
        }
    }

    if (view.screen == UI_SCREEN_PLAYER && view.has_status)
    {
        ui_set_status(view.status);
    }

    s_rendered_screen = view.screen;
    s_has_rendered_screen = true;
}

static void present_transfer(void)
{
    /*
     * Keep pending updates until the transfer screen is visible.
     * These screen variables are owned by the LVGL callback.
     */
    if (!s_has_rendered_screen ||
        s_rendered_screen != UI_SCREEN_TRANSFER)
    {
        return;
    }

    transfer_view_t view;

    portENTER_CRITICAL(&s_transfer_lock);

    view = s_transfer_view;

    s_transfer_view.status_dirty = false;
    s_transfer_view.progress_dirty = false;

    portEXIT_CRITICAL(&s_transfer_lock);

    if (view.status_dirty)
    {
        ui_set_transfer_status(view.status);
    }

    if (view.progress_dirty)
    {
        ui_update_transfer_progress(
            view.done_bytes,
            view.total_bytes);
    }
}

static void presenter_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    present_screen();
    present_controls();
    present_transfer();

    playback_view_t view;

    if (xQueueReceive(s_playback_queue, &view, 0) != pdTRUE)
    {
        return;
    }

    if (!view.track_valid)
    {
        return;
    }

    bool track_changed =
        !s_has_rendered_track ||
        view.track_revision != s_rendered_track_revision;

    if (track_changed)
    {
        ui_notify_track_started(
            view.path,
            view.cover_src,
            view.track_index,
            view.track_count,
            view.duration_sec);
    }

    if (track_changed || view.playing != s_rendered_playing)
    {
        ui_update_playback(view.playing);
    }

    if (track_changed || view.elapsed_sec != s_rendered_elapsed_sec)
    {
        ui_update_elapsed(view.elapsed_sec);
    }

    s_has_rendered_track = true;
    s_rendered_track_revision = view.track_revision;
    s_rendered_elapsed_sec = view.elapsed_sec;
    s_rendered_playing = view.playing;
}

esp_err_t ui_presenter_init(void)
{
    if (s_playback_queue != NULL ||
        s_controls_queue != NULL ||
        s_screen_queue != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_ERR_NO_MEM;

    s_playback_queue = xQueueCreate(1, sizeof(playback_view_t));

    if (s_playback_queue == NULL)
    {
        goto fail;
    }

    s_controls_queue = xQueueCreate(1, sizeof(controls_view_t));

    if (s_controls_queue == NULL)
    {
        goto fail;
    }

    s_screen_queue = xQueueCreate(1, sizeof(screen_view_t));

    if (s_screen_queue == NULL)
    {
        goto fail;
    }

    if (!lvgl_port_lock(1000))
    {
        ret = ESP_ERR_TIMEOUT;
        goto fail;
    }

    s_presenter_timer = lv_timer_create(
        presenter_timer_cb,
        33,
        NULL);

    lvgl_port_unlock();

    if (s_presenter_timer == NULL)
    {
        goto fail;
    }

    return ESP_OK;

fail:
    if (s_screen_queue != NULL)
    {
        vQueueDelete(s_screen_queue);
        s_screen_queue = NULL;
    }

    if (s_controls_queue != NULL)
    {
        vQueueDelete(s_controls_queue);
        s_controls_queue = NULL;
    }

    if (s_playback_queue != NULL)
    {
        vQueueDelete(s_playback_queue);
        s_playback_queue = NULL;
    }

    return ret;
}

esp_err_t ui_presenter_publish_playback(const playback_view_t *view)
{
    if (view == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_playback_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueOverwrite(s_playback_queue, view) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t ui_presenter_publish_controls(const controls_view_t *view)
{
    if (view == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_controls_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueOverwrite(s_controls_queue, view) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t ui_presenter_show_screen(
    ui_screen_t screen,
    const char *status)
{
    switch (screen)
    {
    case UI_SCREEN_BOOT:
    case UI_SCREEN_PLAYER:
    case UI_SCREEN_TRANSFER:
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (s_screen_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    screen_view_t view = {
        .screen = screen,
        .has_status = (status != NULL),
    };

    if (status != NULL)
    {
        snprintf(view.status, sizeof(view.status), "%s", status);
    }

    return xQueueOverwrite(s_screen_queue, &view) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

void ui_presenter_set_transfer_status(const char *status)
{
    if (status == NULL)
    {
        return;
    }

    /* Format outside the critical section. */
    char text[TRANSFER_STATUS_MAX_LEN] = {0};
    snprintf(text, sizeof(text), "%s", status);

    portENTER_CRITICAL(&s_transfer_lock);

    memcpy(s_transfer_view.status, text, sizeof(text));
    s_transfer_view.status_dirty = true;

    portEXIT_CRITICAL(&s_transfer_lock);
}

void ui_presenter_update_transfer_progress(
    uint32_t done_bytes,
    uint32_t total_bytes)
{
    portENTER_CRITICAL(&s_transfer_lock);

    s_transfer_view.done_bytes = done_bytes;
    s_transfer_view.total_bytes = total_bytes;
    s_transfer_view.progress_dirty = true;

    portEXIT_CRITICAL(&s_transfer_lock);
}

void ui_presenter_set_transfer_state(
    const char *status,
    uint32_t done_bytes,
    uint32_t total_bytes)
{
    if (status == NULL)
    {
        return;
    }

    char text[TRANSFER_STATUS_MAX_LEN] = {0};
    snprintf(text, sizeof(text), "%s", status);

    portENTER_CRITICAL(&s_transfer_lock);

    memcpy(s_transfer_view.status, text, sizeof(text));
    s_transfer_view.done_bytes = done_bytes;
    s_transfer_view.total_bytes = total_bytes;

    s_transfer_view.status_dirty = true;
    s_transfer_view.progress_dirty = true;

    portEXIT_CRITICAL(&s_transfer_lock);
}