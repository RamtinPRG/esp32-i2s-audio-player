/* main/audio_player.c */
#include "audio_player.h"

#include "app_config.h"
#include "audio_output.h"
#include "ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_PLAYER";

typedef struct
{
    uint8_t *data;
    size_t len;
} audio_buffer_t;

/* --------------------------------------------------------------------------
 * Internal audio player state
 * -------------------------------------------------------------------------- */
static i2s_chan_handle_t s_tx_chan;

static QueueHandle_t s_free_buffer_queue = NULL;
static QueueHandle_t s_audio_data_queue = NULL;
static QueueHandle_t s_player_cmd_queue = NULL;

static playlist_t *s_playlist = NULL;

static bool s_initialized = false;

static volatile bool s_playing = true;
static volatile uint8_t s_volume = 100;
static volatile bool s_mute = false;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */
static void flush_audio_data_queue(void)
{
    audio_buffer_t tmp;

    /*
     * Move any queued full audio buffers back to the free queue.
     *
     * This is used for:
     * - pause
     * - next track
     * - previous track
     *
     * A few samples may still be in the I2S DMA, but this keeps the skip
     * latency low without needing to tear down the I2S channel.
     */
    while (xQueueReceive(s_audio_data_queue, &tmp, 0) == pdTRUE)
    {
        xQueueSend(s_free_buffer_queue, &tmp, pdMS_TO_TICKS(10));
    }
}

static void apply_volume_to_buffer(audio_buffer_t *buf)
{
    if (buf == NULL || buf->data == NULL || buf->len == 0)
    {
        return;
    }

    bool mute = s_mute;
    uint8_t volume = s_volume;

    if (mute || volume == 0)
    {
        memset(buf->data, 0, buf->len);
        return;
    }

    if (volume >= 100)
    {
        return;
    }

    int16_t *samples = (int16_t *)buf->data;
    size_t sample_count = buf->len / sizeof(int16_t);

    /*
     * Simple perceptual-ish volume curve:
     *
     *     gain = volume^2 / 100
     *     out  = in * gain / 100
     *
     * Later we can optimize this with a cached Q16 gain.
     */
    int32_t gain = volume * volume / 100;

    for (size_t i = 0; i < sample_count; i++)
    {
        samples[i] = (int16_t)(((int32_t)samples[i] * gain) / 100);
    }
}

/* --------------------------------------------------------------------------
 * SD read task
 * -------------------------------------------------------------------------- */
static void sd_read_task(void *arg)
{
    (void)arg;

    audio_buffer_t buf;
    FILE *f = NULL;

    uint32_t track_bytes_read = 0;
    uint32_t last_elapsed_sec = 0;

    while (1)
    {
        /* ------------------------------------------------------------
         * Handle player commands from encoder/input task
         * ------------------------------------------------------------ */
        player_cmd_t cmd;

        while (xQueueReceive(s_player_cmd_queue, &cmd, 0) == pdTRUE)
        {
            if (cmd == PLAYER_CMD_NEXT || cmd == PLAYER_CMD_PREV)
            {
                if (f != NULL)
                {
                    fclose(f);
                    f = NULL;
                }

                flush_audio_data_queue();

                if (cmd == PLAYER_CMD_NEXT)
                {
                    s_playlist->current_index =
                        (s_playlist->current_index + 1) % s_playlist->count;
                }
                else
                {
                    s_playlist->current_index =
                        (s_playlist->current_index - 1 + s_playlist->count) %
                        s_playlist->count;
                }
            }
            else if (cmd == PLAYER_CMD_TOGGLE_PLAY)
            {
                s_playing = !s_playing;

                if (lvgl_port_lock(500))
                {
                    ui_update_playback(s_playing);
                    lvgl_port_unlock();
                }
            }
        }

        /* ------------------------------------------------------------
         * Open current track if needed
         * ------------------------------------------------------------ */
        if (f == NULL)
        {
            const char *filepath = s_playlist->files[s_playlist->current_index];

            ESP_LOGI(
                TAG,
                "Opening track %d/%d: %s",
                s_playlist->current_index + 1,
                s_playlist->count,
                filepath);

            f = fopen(filepath, "rb");

            if (f == NULL)
            {
                ESP_LOGE(TAG, "Failed to open file: %s", filepath);

                s_playlist->current_index =
                    (s_playlist->current_index + 1) % s_playlist->count;

                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            /* Get file size for logging */
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);

            /*
             * Calculate duration:
             * 44100 Hz * 2 channels * 2 bytes = 176400 bytes/sec
             */
            float duration_sec_f = size / 176400.0f;
            uint32_t duration_sec = (uint32_t)duration_sec_f;

            int mins = (int)duration_sec_f / 60;
            int secs = (int)duration_sec_f % 60;

            ESP_LOGI(
                TAG,
                "▶ Started playing: %s (%.2f MB, %02d:%02d)",
                filepath,
                size / (1024.0f * 1024.0f),
                mins,
                secs);

            track_bytes_read = 0;
            last_elapsed_sec = 0;

            /* Check for corresponding .bmp cover art */
            const void *img_arg = NULL;

            if (s_playlist->covers[s_playlist->current_index].valid)
            {
                img_arg = &s_playlist->covers[s_playlist->current_index].dsc;
            }

            /* Thread-safe UI update */
            if (lvgl_port_lock(500))
            {
                ui_notify_track_started(
                    filepath,
                    img_arg,
                    s_playlist->current_index + 1,
                    s_playlist->count,
                    duration_sec);

                /*
                 * If the user changed track while paused, keep the UI in the
                 * paused state.
                 */
                ui_update_playback(s_playing);

                lvgl_port_unlock();
            }
        }

        /* ------------------------------------------------------------
         * If paused, send silence buffers to flush I2S DMA
         * ------------------------------------------------------------ */
        if (!s_playing)
        {
            if (xQueueReceive(s_free_buffer_queue, &buf, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                memset(buf.data, 0, AUDIO_BUFF_SIZE);
                buf.len = AUDIO_BUFF_SIZE;

                if (xQueueSend(s_audio_data_queue, &buf, pdMS_TO_TICKS(10)) != pdTRUE)
                {
                    /* Return buffer to free queue if send timed out */
                    xQueueSend(s_free_buffer_queue, &buf, 0);
                }
            }

            continue;
        }

        /* ------------------------------------------------------------
         * Get an empty buffer
         * ------------------------------------------------------------ */
        if (xQueueReceive(s_free_buffer_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* ------------------------------------------------------------
         * Read from SD card
         * ------------------------------------------------------------ */
        size_t bytes_read = fread(buf.data, 1, AUDIO_BUFF_SIZE, f);

        if (bytes_read == 0)
        {
            ESP_LOGI(
                TAG,
                "⏹ Finished playing: %s",
                s_playlist->files[s_playlist->current_index]);

            /* Thread-safe UI update */
            if (lvgl_port_lock(500))
            {
                ui_notify_track_finished(
                    s_playlist->files[s_playlist->current_index]);

                lvgl_port_unlock();
            }

            fclose(f);
            f = NULL;

            /* Move to next track */
            s_playlist->current_index =
                (s_playlist->current_index + 1) % s_playlist->count;

            /* Return the empty buffer to the free queue */
            xQueueSend(s_free_buffer_queue, &buf, 0);

            /*
             * Small delay to let I2S DMA finish playing the last buffers
             * smoothly.
             */
            vTaskDelay(pdMS_TO_TICKS(50));

            continue;
        }

        buf.len = bytes_read;
        track_bytes_read += bytes_read;

        uint32_t current_elapsed_sec = track_bytes_read / 176400;

        if (current_elapsed_sec != last_elapsed_sec)
        {
            /*
             * Use a short timeout so UI locking doesn't stutter the audio DMA.
             *
             * Later we will move this to a UI event queue.
             */
            if (lvgl_port_lock(10))
            {
                ui_update_elapsed(current_elapsed_sec);
                lvgl_port_unlock();

                last_elapsed_sec = current_elapsed_sec;
            }
        }

        /* Send filled buffer to I2S writer */
        if (xQueueSend(s_audio_data_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            ESP_LOGE(TAG, "Failed to send audio buffer to data queue");
        }
    }
}

/* --------------------------------------------------------------------------
 * I2S write task
 * -------------------------------------------------------------------------- */
static void i2s_write_task(void *arg)
{
    (void)arg;

    audio_buffer_t buf;

    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    while (1)
    {
        if (xQueueReceive(s_audio_data_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* Apply software volume/mute */
        apply_volume_to_buffer(&buf);

        size_t offset = 0;

        while (offset < buf.len)
        {
            size_t bytes_written = 0;

            esp_err_t ret = i2s_channel_write(
                s_tx_chan,
                buf.data + offset,
                buf.len - offset,
                &bytes_written,
                portMAX_DELAY);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
                break;
            }

            offset += bytes_written;
        }

        /* Return buffer to read task */
        xQueueSend(s_free_buffer_queue, &buf, 0);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
esp_err_t audio_player_start(playlist_t *playlist, QueueHandle_t player_cmd_queue)
{
    if (playlist == NULL || player_cmd_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_playlist = playlist;
    s_player_cmd_queue = player_cmd_queue;

    if (!s_initialized)
    {
        ESP_LOGI(TAG, "Initializing audio player");

        esp_err_t ret = audio_output_init(&s_tx_chan);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize audio output");
            return ret;
        }

        /* Create buffer queues */
        s_free_buffer_queue = xQueueCreate(
            AUDIO_BUFFER_COUNT,
            sizeof(audio_buffer_t));

        s_audio_data_queue = xQueueCreate(
            AUDIO_BUFFER_COUNT,
            sizeof(audio_buffer_t));

        assert(s_free_buffer_queue != NULL);
        assert(s_audio_data_queue != NULL);

        /* Allocate DMA-capable audio buffers */
        for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
        {
            uint8_t *mem = heap_caps_calloc(
                AUDIO_BUFF_SIZE,
                1,
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

            if (mem == NULL)
            {
                ESP_LOGE(TAG, "Failed to allocate DMA-capable audio buffer");
                abort();
            }

            audio_buffer_t buf = {
                .data = mem,
                .len = 0,
            };

            xQueueSend(s_free_buffer_queue, &buf, 0);
        }

        /* Create audio tasks */
        xTaskCreate(
            sd_read_task,
            "sd_read_task",
            8192,
            NULL,
            6,
            NULL);

        xTaskCreate(
            i2s_write_task,
            "i2s_write_task",
            4096,
            NULL,
            5,
            NULL);

        s_initialized = true;
    }

    s_playing = true;

    return ESP_OK;
}

void audio_player_set_playback(bool playing)
{
    s_playing = playing;
}

bool audio_player_is_playing(void)
{
    return s_playing;
}

void audio_player_set_volume(uint8_t volume, bool mute)
{
    if (volume > 100)
    {
        volume = 100;
    }

    s_volume = volume;
    s_mute = mute;
}

uint8_t audio_player_get_volume(void)
{
    return s_volume;
}

bool audio_player_is_muted(void)
{
    return s_mute;
}