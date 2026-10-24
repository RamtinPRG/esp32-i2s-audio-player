/* main/app_mode.c */
#include <stdatomic.h>
#include "app_controller.h"

#include "app_config.h"
#include "audio_player.h"
#include "http_upload.h"
#include "playlist.h"
#include "wifi_transfer.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "ui_presenter.h"

static const char *TAG = "APP_CONTROLLER";

/* --------------------------------------------------------------------------
 * Internal application state
 * -------------------------------------------------------------------------- */
typedef enum
{
    APP_MESSAGE_COMMAND = 0,
    APP_MESSAGE_INPUT,
} app_message_type_t;

typedef struct
{
    app_message_type_t type;

    /*
     * For input messages: reject events from a different
     * application mode when they are eventually processed.
     */
    app_mode_t input_mode;

    union
    {
        app_cmd_t command;
        input_event_t input;
    } data;
} app_message_t;

static bool s_volume_overlay_visible = false;
static int64_t s_last_volume_activity_us = 0;

static QueueHandle_t s_player_cmd_queue = NULL;
static QueueHandle_t s_app_cmd_queue = NULL;

static atomic_int s_app_mode = ATOMIC_VAR_INIT(APP_MODE_BOOT_MENU);

static boot_option_t s_boot_option = BOOT_OPT_PLAYER;

static uint8_t s_volume = 100;
static bool s_mute = false;

static playlist_t s_playlist;

static void set_mode(app_mode_t mode)
{
    atomic_store(&s_app_mode, (int)mode);
}

static void publish_controls_view(void)
{
    controls_view_t view = {
        .upload_selected = (s_boot_option == BOOT_OPT_UPLOAD),
        .volume_overlay_visible = s_volume_overlay_visible,
        .volume = s_volume,
        .muted = s_mute,
    };

    esp_err_t ret = ui_presenter_publish_controls(&view);

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to publish controls view: %s",
            esp_err_to_name(ret));
    }
}

static void present_screen(
    ui_screen_t screen,
    const char *status)
{
    esp_err_t ret = ui_presenter_show_screen(screen, status);

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to publish screen: %s",
            esp_err_to_name(ret));
    }
}

static void apply_volume_and_mute(void)
{
    audio_player_set_volume(s_volume, s_mute);
    publish_controls_view();
}

static void change_volume(int delta)
{
    int volume = (int)s_volume + delta;

    if (volume < 0)
    {
        volume = 0;
    }
    else if (volume > 100)
    {
        volume = 100;
    }

    s_volume = (uint8_t)volume;

    /* Preserve the existing behavior: rotation unmutes playback. */
    s_mute = false;

    apply_volume_and_mute();
}

static void show_volume_overlay(bool visible)
{
    s_volume_overlay_visible = visible;

    if (visible)
    {
        s_last_volume_activity_us = esp_timer_get_time();
    }

    publish_controls_view();
}

static void check_volume_overlay_timeout(void)
{
    if (!s_volume_overlay_visible)
    {
        return;
    }

    int64_t elapsed =
        esp_timer_get_time() - s_last_volume_activity_us;

    if (elapsed >= VOLUME_AUTO_HIDE_US)
    {
        show_volume_overlay(false);
    }
}

/*
 * Returns true when the input produces an application command.
 * Overlay-only actions are handled here and return false.
 */
static bool interpret_input(
    input_event_t event,
    app_mode_t mode,
    app_cmd_t *command)
{
    switch (mode)
    {
    case APP_MODE_BOOT_MENU:
        switch (event)
        {
        case INPUT_EVT_ROTATE_CW:
        case INPUT_EVT_ROTATE_CCW:
            *command = APP_CMD_TOGGLE_BOOT_SELECTION;
            return true;

        case INPUT_EVT_SHORT_PRESS:
            *command = APP_CMD_CONFIRM_BOOT_SELECTION;
            return true;

        default:
            return false;
        }

    case APP_MODE_TRANSFER:
        if (event == INPUT_EVT_LONG_PRESS)
        {
            *command = APP_CMD_FINISH_TRANSFER;
            return true;
        }
        return false;

    case APP_MODE_PLAYER:
        if (event == INPUT_EVT_LONG_PRESS)
        {
            show_volume_overlay(!s_volume_overlay_visible);
            return false;
        }

        if (s_volume_overlay_visible)
        {
            switch (event)
            {
            case INPUT_EVT_ROTATE_CW:
                *command = APP_CMD_VOLUME_UP;
                break;

            case INPUT_EVT_ROTATE_CCW:
                *command = APP_CMD_VOLUME_DOWN;
                break;

            case INPUT_EVT_SHORT_PRESS:
                *command = APP_CMD_TOGGLE_MUTE;
                break;

            default:
                return false;
            }

            s_last_volume_activity_us = esp_timer_get_time();
            return true;
        }

        switch (event)
        {
        case INPUT_EVT_ROTATE_CW:
            *command = APP_CMD_NEXT_TRACK;
            return true;

        case INPUT_EVT_ROTATE_CCW:
            *command = APP_CMD_PREVIOUS_TRACK;
            return true;

        case INPUT_EVT_SHORT_PRESS:
            *command = APP_CMD_TOGGLE_PLAYBACK;
            return true;

        default:
            return false;
        }

    default:
        return false;
    }
}

/*
 * Returns true if this is a volume command, including commands
 * deliberately ignored outside player mode.
 */
static bool handle_volume_command(app_cmd_t cmd, app_mode_t mode)
{
    switch (cmd)
    {
    case APP_CMD_VOLUME_UP:
    case APP_CMD_VOLUME_DOWN:
    case APP_CMD_TOGGLE_MUTE:
        break;

    default:
        return false;
    }

    if (mode != APP_MODE_PLAYER)
    {
        ESP_LOGW(
            TAG,
            "Ignoring volume command %d in mode %d",
            (int)cmd,
            (int)mode);
        return true;
    }

    switch (cmd)
    {
    case APP_CMD_VOLUME_UP:
        change_volume(VOLUME_STEP);
        break;

    case APP_CMD_VOLUME_DOWN:
        change_volume(-VOLUME_STEP);
        break;

    case APP_CMD_TOGGLE_MUTE:
        s_mute = !s_mute;
        apply_volume_and_mute();
        break;

    default:
        break;
    }

    return true;
}

/*
 * Returns true if cmd is a playback command, including commands
 * deliberately ignored outside player mode.
 */
static bool handle_playback_command(app_cmd_t cmd, app_mode_t mode)
{
    player_cmd_t player_cmd;

    switch (cmd)
    {
    case APP_CMD_NEXT_TRACK:
        player_cmd = PLAYER_CMD_NEXT;
        break;

    case APP_CMD_PREVIOUS_TRACK:
        player_cmd = PLAYER_CMD_PREV;
        break;

    case APP_CMD_TOGGLE_PLAYBACK:
        player_cmd = PLAYER_CMD_TOGGLE_PLAY;
        break;

    default:
        return false;
    }

    if (mode != APP_MODE_PLAYER)
    {
        ESP_LOGW(
            TAG,
            "Ignoring playback command %d in mode %d",
            (int)cmd,
            (int)mode);
        return true;
    }

    if (s_player_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "Player command queue is unavailable");
        return true;
    }

    if (xQueueSend(s_player_cmd_queue, &player_cmd, 0) != pdTRUE)
    {
        ESP_LOGW(
            TAG,
            "Player command queue full; dropped command %d",
            (int)player_cmd);
    }

    return true;
}

/* --------------------------------------------------------------------------
 * Mode transitions
 * -------------------------------------------------------------------------- */
static void start_player_mode(bool from_transfer)
{
    (void)from_transfer;

    set_mode(APP_MODE_BUSY);

    present_screen(UI_SCREEN_PLAYER, "Scanning PCM files...");

    playlist_free(&s_playlist);
    esp_err_t ret = playlist_build(&s_playlist);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "No playable PCM files found");

        present_screen(UI_SCREEN_PLAYER, "No PCM files found!");

        vTaskDelay(pdMS_TO_TICKS(1500));

        present_screen(UI_SCREEN_BOOT, NULL);

        set_mode(APP_MODE_BOOT_MENU);
        return;
    }

    present_screen(UI_SCREEN_PLAYER, "Loading cover art...");

    playlist_preload_covers(&s_playlist);

    present_screen(UI_SCREEN_PLAYER, "Starting playback...");

    /* Publish gain before startup can release the output worker. */
    audio_player_set_volume(s_volume, s_mute);

    esp_err_t audio_ret = audio_player_start(&s_playlist, s_player_cmd_queue);

    if (audio_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start audio player");

        present_screen(UI_SCREEN_PLAYER, "Audio init failed!");

        vTaskDelay(pdMS_TO_TICKS(1500));

        present_screen(UI_SCREEN_BOOT, NULL);

        set_mode(APP_MODE_BOOT_MENU);
        return;
    }

    set_mode(APP_MODE_PLAYER);
}

static void enter_transfer_from_boot_menu(void)
{
    set_mode(APP_MODE_BUSY);

    ui_presenter_set_transfer_state("Starting Wi-Fi...", 0, 0);
    present_screen(UI_SCREEN_TRANSFER, NULL);

    if (wifi_transfer_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start Wi-Fi transfer mode");

        wifi_transfer_stop();

        ui_presenter_set_transfer_status("Wi-Fi start failed!");

        vTaskDelay(pdMS_TO_TICKS(1500));

        present_screen(UI_SCREEN_BOOT, NULL);

        set_mode(APP_MODE_BOOT_MENU);
        return;
    }

    if (http_upload_server_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP upload server");

        http_upload_server_stop();
        wifi_transfer_stop();

        ui_presenter_set_transfer_status("HTTP start failed!");

        vTaskDelay(pdMS_TO_TICKS(1500));

        present_screen(UI_SCREEN_BOOT, NULL);

        set_mode(APP_MODE_BOOT_MENU);
        return;
    }

    ui_presenter_set_transfer_status("Wi-Fi & HTTP ready.");

    set_mode(APP_MODE_TRANSFER);
}

static void finish_transfer_and_enter_player(void)
{
    set_mode(APP_MODE_BUSY);

    ui_presenter_set_transfer_status("Stopping services...");

    http_upload_server_stop();
    wifi_transfer_stop();

    /* Give the system time to finish cleanup */
    vTaskDelay(pdMS_TO_TICKS(500));

    ui_presenter_set_transfer_status("Exiting transfer mode...");

    vTaskDelay(pdMS_TO_TICKS(250));

    start_player_mode(true);
}

/* --------------------------------------------------------------------------
 * Application control task
 * -------------------------------------------------------------------------- */
static void app_control_task(void *arg)
{
    (void)arg;

    publish_controls_view();
    present_screen(UI_SCREEN_BOOT, NULL);

    app_message_t message;

    while (1)
    {
        /*
         * While the overlay is visible, wake periodically to check
         * inactivity. Otherwise sleep until a message arrives.
         */
        TickType_t wait_ticks = portMAX_DELAY;

        if (s_volume_overlay_visible)
        {
            wait_ticks = pdMS_TO_TICKS(50);

            if (wait_ticks == 0)
            {
                wait_ticks = 1;
            }
        }

        BaseType_t received =
            xQueueReceive(s_app_cmd_queue, &message, wait_ticks);

        check_volume_overlay_timeout();

        if (received != pdTRUE)
        {
            continue;
        }

        app_mode_t mode = app_controller_get_mode();
        app_cmd_t cmd;

        if (message.type == APP_MESSAGE_INPUT)
        {
            if (message.input_mode != mode)
            {
                continue;
            }

            if (!interpret_input(message.data.input, mode, &cmd))
            {
                continue;
            }
        }
        else if (message.type == APP_MESSAGE_COMMAND)
        {
            cmd = message.data.command;
        }
        else
        {
            ESP_LOGW(TAG, "Unknown application message type");
            continue;
        }

        if (handle_volume_command(cmd, mode))
        {
            continue;
        }

        if (handle_playback_command(cmd, mode))
        {
            continue;
        }

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

            publish_controls_view();
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
    /* Startup is called serially from app_main, before input is enabled. */
    if (s_player_cmd_queue != NULL || s_app_cmd_queue != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_player_cmd_queue = xQueueCreate(8, sizeof(player_cmd_t));

    if (s_player_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create player command queue");
        return ESP_ERR_NO_MEM;
    }

    s_app_cmd_queue = xQueueCreate(8, sizeof(app_message_t));

    if (s_app_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create application command queue");
        goto fail;
    }

    if (xTaskCreate(
            app_control_task,
            "app_ctrl",
            10240,
            NULL,
            6,
            NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create application control task");
        goto fail;
    }

    ESP_LOGI(TAG, "Application controller started");
    return ESP_OK;

fail:
    /* No controller task was created, so neither queue has a consumer. */
    if (s_app_cmd_queue != NULL)
    {
        vQueueDelete(s_app_cmd_queue);
        s_app_cmd_queue = NULL;
    }

    vQueueDelete(s_player_cmd_queue);
    s_player_cmd_queue = NULL;

    return ESP_ERR_NO_MEM;
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
    case APP_CMD_VOLUME_UP:
    case APP_CMD_VOLUME_DOWN:
    case APP_CMD_TOGGLE_MUTE:
    case APP_CMD_NEXT_TRACK:
    case APP_CMD_PREVIOUS_TRACK:
    case APP_CMD_TOGGLE_PLAYBACK:
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (s_app_cmd_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    app_message_t message = {
        .type = APP_MESSAGE_COMMAND,
        .data.command = cmd,
    };

    return xQueueSend(s_app_cmd_queue, &message, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t app_controller_post_input(input_event_t event)
{
    switch (event)
    {
    case INPUT_EVT_ROTATE_CW:
    case INPUT_EVT_ROTATE_CCW:
    case INPUT_EVT_SHORT_PRESS:
    case INPUT_EVT_LONG_PRESS:
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (s_app_cmd_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    app_mode_t mode = app_controller_get_mode();

    /* Input during a transition is intentionally ignored. */
    if (mode == APP_MODE_BUSY)
    {
        return ESP_OK;
    }

    app_message_t message = {
        .type = APP_MESSAGE_INPUT,
        .input_mode = mode,
        .data.input = event,
    };

    return xQueueSend(s_app_cmd_queue, &message, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}