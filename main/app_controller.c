/* main/app_mode.c */
#include <stdatomic.h>
#include "app_controller.h"

#include "app_config.h"
#include "audio_player.h"
#include "http_upload.h"
#include "playlist.h"
#include "wifi_transfer.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "APP_CONTROLLER";

/* --------------------------------------------------------------------------
 * Internal application state
 * -------------------------------------------------------------------------- */
static QueueHandle_t s_player_cmd_queue = NULL;
static QueueHandle_t s_app_cmd_queue = NULL;

static atomic_int s_app_mode = ATOMIC_VAR_INIT(APP_MODE_BOOT_MENU);

static boot_option_t s_boot_option = BOOT_OPT_PLAYER;

static volatile uint8_t s_volume = 100;
static volatile bool s_mute = false;

static playlist_t s_playlist;

static void set_mode(app_mode_t mode)
{
    atomic_store(&s_app_mode, (int)mode);
}
/* --------------------------------------------------------------------------
 * Mode transitions
 * -------------------------------------------------------------------------- */
static void start_player_mode(bool from_transfer)
{
    (void)from_transfer;

    set_mode(APP_MODE_BUSY);

    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(false);
        ui_show_transfer_screen(false, NULL, NULL, NULL);
        ui_set_status("Scanning PCM files...");
        lvgl_port_unlock();
    }

    playlist_free(&s_playlist);
    esp_err_t ret = playlist_build(&s_playlist);

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
            ui_set_boot_selection(s_boot_option);
            lvgl_port_unlock();
        }

        set_mode(APP_MODE_BOOT_MENU);
        return;
    }

    if (lvgl_port_lock(500))
    {
        ui_set_status("Loading cover art...");
        lvgl_port_unlock();
    }

    playlist_preload_covers(&s_playlist);

    if (lvgl_port_lock(500))
    {
        ui_set_status("Starting playback...");
        lvgl_port_unlock();
    }

    esp_err_t audio_ret = audio_player_start(&s_playlist, s_player_cmd_queue);

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
            ui_set_boot_selection(s_boot_option);
            lvgl_port_unlock();
        }

        set_mode(APP_MODE_BOOT_MENU);
        return;
    }

    audio_player_set_volume(s_volume, s_mute);

    set_mode(APP_MODE_PLAYER);
}

static void enter_transfer_from_boot_menu(void)
{
    set_mode(APP_MODE_BUSY);

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
            ui_set_boot_selection(s_boot_option);
            lvgl_port_unlock();
        }

        set_mode(APP_MODE_BOOT_MENU);
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
            ui_set_boot_selection(s_boot_option);
            lvgl_port_unlock();
        }

        set_mode(APP_MODE_BOOT_MENU);
        return;
    }

    if (lvgl_port_lock(500))
    {
        ui_set_transfer_status("Wi-Fi & HTTP ready.");
        lvgl_port_unlock();
    }

    set_mode(APP_MODE_TRANSFER);
}

static void finish_transfer_and_enter_player(void)
{
    set_mode(APP_MODE_BUSY);

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
 * -------------------------------------------------------------------------- */
static void app_control_task(void *arg)
{
    (void)arg;

    /*
     * The controller owns the initial boot-menu presentation.
     * Display initialization has already completed in app_main().
     */
    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(true);
        ui_set_boot_selection(s_boot_option);
        lvgl_port_unlock();
    }

    app_cmd_t cmd;

    while (1)
    {
        if (xQueueReceive(s_app_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        app_mode_t mode = app_controller_get_mode();

        switch (cmd)
        {
        case APP_CMD_TOGGLE_BOOT_SELECTION:
            if (mode != APP_MODE_BOOT_MENU)
            {
                break;
            }

            s_boot_option =
                (s_boot_option == BOOT_OPT_PLAYER)
                    ? BOOT_OPT_UPLOAD
                    : BOOT_OPT_PLAYER;

            if (lvgl_port_lock(200))
            {
                ui_set_boot_selection(s_boot_option);
                lvgl_port_unlock();
            }
            continue;

        case APP_CMD_CONFIRM_BOOT_SELECTION:
            if (mode != APP_MODE_BOOT_MENU)
            {
                break;
            }

            if (s_boot_option == BOOT_OPT_PLAYER)
            {
                ESP_LOGI(TAG, "Boot selection: player");
                start_player_mode(false);
            }
            else
            {
                ESP_LOGI(TAG, "Boot selection: transfer");
                enter_transfer_from_boot_menu();
            }
            continue;

        case APP_CMD_START_PLAYER:
            if (mode != APP_MODE_BOOT_MENU)
            {
                break;
            }

            ESP_LOGI(TAG, "App command: START_PLAYER");
            start_player_mode(false);
            continue;

        case APP_CMD_START_TRANSFER:
            if (mode != APP_MODE_BOOT_MENU)
            {
                break;
            }

            ESP_LOGI(TAG, "App command: START_TRANSFER");
            enter_transfer_from_boot_menu();
            continue;

        case APP_CMD_FINISH_TRANSFER:
            if (mode != APP_MODE_TRANSFER)
            {
                break;
            }

            ESP_LOGI(TAG, "App command: FINISH_TRANSFER");
            finish_transfer_and_enter_player();
            continue;

        default:
            ESP_LOGW(TAG, "Unknown app command: %d", (int)cmd);
            continue;
        }

        ESP_LOGW(
            TAG,
            "Ignoring app command %d in mode %d",
            (int)cmd,
            (int)mode);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
esp_err_t app_controller_start(void)
{
    s_player_cmd_queue = xQueueCreate(8, sizeof(player_cmd_t));
    s_app_cmd_queue = xQueueCreate(8, sizeof(app_cmd_t));

    if (s_player_cmd_queue == NULL || s_app_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create application queues");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(
        app_control_task,
        "app_ctrl",
        10240,
        NULL,
        6,
        NULL);

    ESP_LOGI(TAG, "Application mode control started");

    return ESP_OK;
}

QueueHandle_t app_controller_get_player_cmd_queue(void)
{
    return s_player_cmd_queue;
}

app_mode_t app_controller_get_mode(void)
{
    return (app_mode_t)atomic_load(&s_app_mode);
}

esp_err_t app_controller_submit(app_cmd_t cmd)
{
    switch (cmd)
    {
    case APP_CMD_START_PLAYER:
    case APP_CMD_START_TRANSFER:
    case APP_CMD_FINISH_TRANSFER:
    case APP_CMD_TOGGLE_BOOT_SELECTION:
    case APP_CMD_CONFIRM_BOOT_SELECTION:
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (s_app_cmd_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueSend(s_app_cmd_queue, &cmd, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

volatile uint8_t *app_controller_get_volume_ptr(void)
{
    return &s_volume;
}

volatile bool *app_controller_get_mute_ptr(void)
{
    return &s_mute;
}