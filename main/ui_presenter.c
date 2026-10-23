#include "ui_presenter.h"

#include "ui.h"

#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include <stdio.h>

#define SCREEN_STATUS_MAX_LEN 96

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

static void presenter_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    present_screen();
    present_controls();

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