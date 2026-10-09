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
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "iot_knob.h"
#include "iot_button.h"
#include "button_gpio.h"

#include "sdkconfig.h"

#include "ui.h"
#include "cover_cache.h"
#include "esp_lvgl_port.h"

#include "app_config.h"
#include "bsp_display.h"
#include "bsp_sdcard.h"
#include "audio_output.h"
#include "playlist.h"
#include "audio_player.h"
#include "app_types.h"
#include "input_knob.h"
#include "http_upload.h"
#include "wifi_transfer.h"

static const char *TAG = "I2S_SD";

/* --------------------------------------------------------------------------
 * Global playlist and audio system state
 * -------------------------------------------------------------------------- */
static playlist_t playlist;

/* --------------------------------------------------------------------------
 * Encoder / UI mode state machine
 * -------------------------------------------------------------------------- */

typedef enum
{
    UI_MODE_TRACK = 0,
    UI_MODE_VOLUME,
} ui_mode_t;

static QueueHandle_t player_cmd_queue = NULL;

static QueueHandle_t app_cmd_queue = NULL;
static volatile app_mode_t app_mode = APP_MODE_BOOT_MENU;
static boot_option_t boot_option = BOOT_OPT_PLAYER;

static volatile uint8_t app_volume = 100;
static volatile bool app_mute = false;

/* --------------------------------------------------------------------------
 * Enter normal Player mode.
 *
 * This scans the SD card, loads cover art, and starts audio playback.
 * If no PCM files are found, it returns to the boot menu.
 * -------------------------------------------------------------------------- */
static void start_player_mode(bool from_transfer)
{
    (void)from_transfer;

    app_mode = APP_MODE_BUSY;

    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(false);
        ui_show_transfer_screen(false, NULL, NULL, NULL);
        ui_set_status("Scanning PCM files...");
        lvgl_port_unlock();
    }

    esp_err_t ret = playlist_build(&playlist);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "No playable PCM files found");

        if (lvgl_port_lock(500))
        {
            ui_set_status("No PCM files found!");
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(1500));

        if (lvgl_port_lock(500))
        {
            ui_show_boot_menu(true);
            ui_set_boot_selection(boot_option);
            lvgl_port_unlock();
        }

        app_mode = APP_MODE_BOOT_MENU;
        return;
    }

    if (lvgl_port_lock(500))
    {
        ui_set_status("Loading cover art...");
        lvgl_port_unlock();
    }

    playlist_preload_covers(&playlist);

    if (lvgl_port_lock(500))
    {
        ui_set_status("Starting playback...");
        lvgl_port_unlock();
    }

    esp_err_t audio_ret = audio_player_start(&playlist, player_cmd_queue);

    if (audio_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start audio player");

        if (lvgl_port_lock(500))
        {
            ui_set_status("Audio init failed!");
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(1500));

        if (lvgl_port_lock(500))
        {
            ui_show_boot_menu(true);
            ui_set_boot_selection(boot_option);
            lvgl_port_unlock();
        }

        app_mode = APP_MODE_BOOT_MENU;
        return;
    }

    audio_player_set_volume(app_volume, app_mute);

    app_mode = APP_MODE_PLAYER;
}

/* --------------------------------------------------------------------------
 * Enter Transfer mode placeholder.
 *
 * Later this will start:
 * - Wi-Fi AP
 * - HTTP upload server
 * - transfer screen UI
 * -------------------------------------------------------------------------- */
static void enter_transfer_from_boot_menu(void)
{
    app_mode = APP_MODE_BUSY;

    audio_player_set_playback(false);

    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(false);

        ui_show_transfer_screen(
            true,
            WIFI_AP_SSID,
            WIFI_AP_PASSWORD,
            WIFI_TRANSFER_URL);

        ui_set_transfer_status("Starting Wi-Fi...");

        lvgl_port_unlock();
    }

    if (wifi_transfer_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start Wi-Fi transfer mode");

        wifi_transfer_stop();

        if (lvgl_port_lock(500))
        {
            ui_set_transfer_status("Wi-Fi start failed!");
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(1500));

        if (lvgl_port_lock(500))
        {
            ui_show_transfer_screen(false, NULL, NULL, NULL);
            ui_show_boot_menu(true);
            ui_set_boot_selection(boot_option);
            lvgl_port_unlock();
        }

        app_mode = APP_MODE_BOOT_MENU;
        return;
    }

    if (http_upload_server_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP upload server");

        http_upload_server_stop();
        wifi_transfer_stop();

        if (lvgl_port_lock(500))
        {
            ui_set_transfer_status("HTTP start failed!");
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(1500));

        if (lvgl_port_lock(500))
        {
            ui_show_transfer_screen(false, NULL, NULL, NULL);
            ui_show_boot_menu(true);
            ui_set_boot_selection(boot_option);
            lvgl_port_unlock();
        }

        app_mode = APP_MODE_BOOT_MENU;
        return;
    }

    if (lvgl_port_lock(500))
    {
        ui_set_transfer_status("Wi-Fi & HTTP ready.");
        lvgl_port_unlock();
    }

    app_mode = APP_MODE_TRANSFER;
}

/* --------------------------------------------------------------------------
 * Finish Transfer mode and enter Player mode.
 * -------------------------------------------------------------------------- */
static void finish_transfer_and_enter_player(void)
{
    app_mode = APP_MODE_BUSY;

    if (lvgl_port_lock(500))
    {
        ui_set_transfer_status("Stopping services...");
        lvgl_port_unlock();
    }

    http_upload_server_stop();
    wifi_transfer_stop();

    /* Give the system time to finish cleanup */
    vTaskDelay(pdMS_TO_TICKS(500));

    if (lvgl_port_lock(500))
    {
        ui_set_transfer_status("Exiting transfer mode...");
        lvgl_port_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(250));

    start_player_mode(true);
}

/* --------------------------------------------------------------------------
 * Application control task
 *
 * This task performs heavy mode transitions:
 * - starting Player mode
 * - starting Transfer mode
 * - finishing Transfer mode
 * -------------------------------------------------------------------------- */
static void app_control_task(void *arg)
{
    (void)arg;

    app_cmd_t cmd;

    while (1)
    {
        if (xQueueReceive(app_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (cmd)
        {
        case APP_CMD_START_PLAYER:
            ESP_LOGI(TAG, "App command: START_PLAYER");
            start_player_mode(false);
            break;

        case APP_CMD_START_TRANSFER:
            ESP_LOGI(TAG, "App command: START_TRANSFER");
            enter_transfer_from_boot_menu();
            break;

        case APP_CMD_FINISH_TRANSFER:
            ESP_LOGI(TAG, "App command: FINISH_TRANSFER");
            finish_transfer_and_enter_player();
            break;

        default:
            ESP_LOGW(TAG, "App command: unknown %d", (int)cmd);
            break;
        }
    }
}

void app_main(void)
{
    /* Initialize display and LVGL UI */
    bsp_display_init();

    /* Create input/application queues */
    player_cmd_queue = xQueueCreate(8, sizeof(player_cmd_t));
    app_cmd_queue = xQueueCreate(8, sizeof(app_cmd_t));

    assert(player_cmd_queue != NULL);
    assert(app_cmd_queue != NULL);

    /* Mount SD card */
    if (lvgl_port_lock(500))
    {
        ui_set_status("Mounting SD card...");
        lvgl_port_unlock();
    }

    ESP_ERROR_CHECK(bsp_sdcard_mount());

    /* Start in boot menu mode */
    app_mode = APP_MODE_BOOT_MENU;

    /* Create application control task */
    xTaskCreate(app_control_task, "app_ctrl", 10240, NULL, 6, NULL);

    /* Initialize encoder/input state machine */
    input_knob_config_t input_cfg = {
        .player_cmd_queue = player_cmd_queue,
        .app_cmd_queue = app_cmd_queue,
        .app_mode = &app_mode,
        .boot_option = &boot_option,
        .volume = &app_volume,
        .mute = &app_mute,
    };

    ESP_ERROR_CHECK(input_knob_init(&input_cfg));

    /* Show boot menu */
    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(true);
        ui_set_boot_selection(boot_option);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Boot menu active");
}