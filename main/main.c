/*
 * ESP-IDF I2S audio player with SD Card Playlist
 *
 * Scans SD card for .pcm files and plays them in alphabetical order.
 *
 * Audio format:
 *   44100 Hz, 16-bit, stereo, raw PCM
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "sdmmc_cmd.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"

#include "sdkconfig.h"
// #include "i2s_example_pins.h"
#include "ui_display.h"

static const char *TAG = "I2S_SD";

/* I2S pins from your existing example header */
#define EXAMPLE_STD_BCLK_IO1 38
#define EXAMPLE_STD_WS_IO1 40
#define EXAMPLE_STD_DOUT_IO1 39
#define EXAMPLE_STD_DIN_IO1 I2S_GPIO_UNUSED

/*
 * SD card SPI pins.
 * IMPORTANT: On ESP32-S3, avoid GPIO19 and GPIO20 if you are using USB!
 * Adjust these to match your actual wiring.
 */
#define SD_PIN_MOSI 5
#define SD_PIN_MISO 4
#define SD_PIN_CLK 6
#define SD_PIN_CS 7

/* Mount point */
#define SD_MOUNT_POINT "/sdcard"

/* Audio buffer settings */
#define AUDIO_BUFF_SIZE 4096
#define AUDIO_BUFFER_COUNT 6

/* Playlist settings */
#define MAX_FILES 128
#define MAX_PATH_LEN 512

static i2s_chan_handle_t tx_chan;
static sdmmc_card_t *sd_card = NULL;

static QueueHandle_t free_buffer_queue = NULL;
static QueueHandle_t audio_data_queue = NULL;

typedef struct
{
    uint8_t *data;
    size_t len;
} audio_buffer_t;

typedef struct
{
    char files[MAX_FILES][MAX_PATH_LEN];
    int count;
    int current_index;
} playlist_t;

/*
 * Mount SD card using SPI mode.
 */
static esp_err_t mount_sdcard(void)
{
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Initializing SD card");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    // Start with a safe speed. Increase to 20000 (20MHz) later if stable.
    // host.max_freq_khz = 10000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AUDIO_BUFF_SIZE,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. Make sure the SD card is FAT32.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");
    sdmmc_card_print_info(stdout, sd_card);

    return ESP_OK;
}

/*
 * Initialize I2S standard mode: 44100 Hz, 16-bit, stereo.
 */
static void i2s_example_init_std(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),

        /* Note: If your DAC expects Philips format, change MSB to PHILIPS */
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),

        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = EXAMPLE_STD_BCLK_IO1,
            .ws = EXAMPLE_STD_WS_IO1,
            .dout = EXAMPLE_STD_DOUT_IO1,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
}

/* Helper for qsort */
static int compare_strings(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/*
 * Scans the SD card for .pcm files and builds a sorted playlist.
 */
static esp_err_t build_playlist(playlist_t *playlist)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to open directory %s", SD_MOUNT_POINT);
        return ESP_FAIL;
    }

    playlist->count = 0;
    struct dirent *entry;

    ESP_LOGI(TAG, "Scanning for .pcm files...");

    while ((entry = readdir(dir)) != NULL && playlist->count < MAX_FILES)
    {
        if (entry->d_type == DT_REG)
        {
            const char *name = entry->d_name;
            int len = strlen(name);
            // Check if it ends with .pcm (case-insensitive)
            if (len > 4 && strcasecmp(name + len - 4, ".pcm") == 0)
            {
                snprintf(playlist->files[playlist->count], MAX_PATH_LEN, "%s/%s", SD_MOUNT_POINT, name);
                playlist->count++;
            }
        }
    }
    closedir(dir);

    if (playlist->count == 0)
    {
        ESP_LOGE(TAG, "No .pcm files found on SD card!");
        return ESP_ERR_NOT_FOUND;
    }

    // Sort the files alphabetically
    qsort(playlist->files, playlist->count, MAX_PATH_LEN, compare_strings);

    ESP_LOGI(TAG, "Found %d PCM file(s):", playlist->count);
    for (int i = 0; i < playlist->count; i++)
    {
        ESP_LOGI(TAG, "  [%d] %s", i + 1, playlist->files[i]);
    }

    playlist->current_index = 0;
    return ESP_OK;
}

/*
 * Task that reads audio data from the SD card playlist.
 */
static void sd_read_task(void *arg)
{
    playlist_t *playlist = (playlist_t *)arg;
    audio_buffer_t buf;
    FILE *f = NULL;

    while (1)
    {
        // If no file is open, open the next one in the playlist
        if (f == NULL)
        {
            const char *filepath = playlist->files[playlist->current_index];
            ESP_LOGI(TAG, "Opening track %d/%d: %s",
                     playlist->current_index + 1, playlist->count, filepath);

            f = fopen(filepath, "rb");
            if (f == NULL)
            {
                ESP_LOGE(TAG, "Failed to open file: %s", filepath);
                playlist->current_index = (playlist->current_index + 1) % playlist->count;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // Get file size for logging
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);

            // Calculate duration: 44100 Hz * 2 channels * 2 bytes = 176400 bytes/sec
            float duration_sec_f = size / 176400.0f;
            uint32_t duration_sec = (uint32_t)duration_sec_f;
            int mins = (int)duration_sec_f / 60;
            int secs = (int)duration_sec_f % 60;
            ESP_LOGI(TAG, "▶ Started playing: %s (%.2f MB, %02d:%02d)",
                     filepath, size / (1024.0f * 1024.0f), mins, secs);

            // Pass duration to UI
            ui_notify_track_started(filepath, playlist->current_index + 1, playlist->count, duration_sec);
        }

        /* Get an empty buffer */
        if (xQueueReceive(free_buffer_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* Read from SD card */
        size_t bytes_read = fread(buf.data, 1, AUDIO_BUFF_SIZE, f);

        if (bytes_read == 0)
        {
            /* End of file reached */
            ESP_LOGI(TAG, "⏹ Finished playing: %s", playlist->files[playlist->current_index]);
            ui_notify_track_finished(playlist->files[playlist->current_index]);
            fclose(f);
            f = NULL;

            /* Move to next track */
            playlist->current_index = (playlist->current_index + 1) % playlist->count;

            /* Return the empty buffer to the free queue */
            xQueueSend(free_buffer_queue, &buf, 0);

            // Small delay to let I2S DMA finish playing the last buffers smoothly
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        buf.len = bytes_read;

        /* Send filled buffer to I2S writer */
        if (xQueueSend(audio_data_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            ESP_LOGE(TAG, "Failed to send audio buffer to data queue");
        }
    }
}

/*
 * Task that writes audio data to I2S.
 */
static void i2s_write_task(void *arg)
{
    audio_buffer_t buf;

    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    while (1)
    {
        if (xQueueReceive(audio_data_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        size_t offset = 0;
        while (offset < buf.len)
        {
            size_t bytes_written = 0;
            esp_err_t ret = i2s_channel_write(
                tx_chan, buf.data + offset, buf.len - offset, &bytes_written, portMAX_DELAY);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
                break;
            }
            offset += bytes_written;
        }

        /* Return buffer to read task */
        xQueueSend(free_buffer_queue, &buf, 0);
    }
}

void app_main(void)
{
    /* Initialize LVGL display first */
    ui_display_init();
    ui_set_status("Mounting SD card...");

    /* Mount SD card */
    ESP_ERROR_CHECK(mount_sdcard());

    ui_set_status("Scanning PCM files...");

    /* Build playlist */
    static playlist_t playlist;
    esp_err_t ret = build_playlist(&playlist);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to build playlist. Halting.");
        ui_set_status("No PCM files found!");
        return;
    }

    ui_set_status("Starting playback...");

    /* Initialize I2S */
    i2s_example_init_std();

    /* Create buffer queues */
    free_buffer_queue = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_buffer_t));
    audio_data_queue = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_buffer_t));

    assert(free_buffer_queue != NULL);
    assert(audio_data_queue != NULL);

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

        xQueueSend(free_buffer_queue, &buf, 0);
    }

    /* Create tasks */
    xTaskCreate(sd_read_task, "sd_read_task", 8192, &playlist, 6, NULL);
    xTaskCreate(i2s_write_task, "i2s_write_task", 4096, NULL, 5, NULL);
}