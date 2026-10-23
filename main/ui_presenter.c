#include "ui_presenter.h"

#include "ui.h"

#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

static QueueHandle_t s_playback_queue = NULL;
static lv_timer_t *s_presenter_timer = NULL;

/* Accessed only from the LVGL timer callback. */
static bool s_has_rendered_track = false;
static uint32_t s_rendered_track_revision = 0;
static uint32_t s_rendered_elapsed_sec = 0;
static bool s_rendered_playing = false;

static void presenter_timer_cb(lv_timer_t *timer)
{
    (void)timer;

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
    if (s_playback_queue != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_playback_queue = xQueueCreate(1, sizeof(playback_view_t));

    if (s_playback_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (!lvgl_port_lock(1000))
    {
        vQueueDelete(s_playback_queue);
        s_playback_queue = NULL;
        return ESP_ERR_TIMEOUT;
    }

    s_presenter_timer = lv_timer_create(
        presenter_timer_cb,
        33,
        NULL);

    lvgl_port_unlock();

    if (s_presenter_timer == NULL)
    {
        vQueueDelete(s_playback_queue);
        s_playback_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
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