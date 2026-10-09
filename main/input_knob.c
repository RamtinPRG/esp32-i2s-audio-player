/* main/input_knob.c */
#include "input_knob.h"

#include "app_config.h"
#include "audio_player.h"
#include "ui.h"

#include <stdint.h>

#include "button_gpio.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "iot_knob.h"

static const char *TAG = "INPUT_KNOB";

typedef enum
{
    INPUT_UI_MODE_TRACK = 0,
    INPUT_UI_MODE_VOLUME,
} input_ui_mode_t;

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */
static input_knob_config_t s_cfg;

static QueueHandle_t s_input_event_queue = NULL;
static esp_timer_handle_t s_volume_autohide_timer = NULL;

static input_ui_mode_t s_ui_mode = INPUT_UI_MODE_TRACK;
static int64_t s_last_volume_activity_us = 0;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */
static void post_input_event(input_event_t evt)
{
    if (s_input_event_queue == NULL)
    {
        return;
    }

    if (xPortInIsrContext() == pdTRUE)
    {
        BaseType_t higher_woken = pdFALSE;

        xQueueSendFromISR(s_input_event_queue, &evt, &higher_woken);

        if (higher_woken)
        {
            portYIELD_FROM_ISR();
        }
    }
    else
    {
        xQueueSend(s_input_event_queue, &evt, 0);
    }
}

static void send_player_cmd(player_cmd_t cmd)
{
    if (s_cfg.player_cmd_queue == NULL)
    {
        return;
    }

    /*
     * Use zero timeout so the input task never blocks.
     * If the queue is full, dropping one encoder command is usually
     * preferable to blocking the UI/input task.
     */
    xQueueSend(s_cfg.player_cmd_queue, &cmd, 0);
}

static void post_app_cmd(app_cmd_t cmd)
{
    if (s_cfg.app_cmd_queue == NULL)
    {
        return;
    }

    if (xPortInIsrContext() == pdTRUE)
    {
        BaseType_t higher_woken = pdFALSE;

        xQueueSendFromISR(s_cfg.app_cmd_queue, &cmd, &higher_woken);

        if (higher_woken)
        {
            portYIELD_FROM_ISR();
        }
    }
    else
    {
        xQueueSend(s_cfg.app_cmd_queue, &cmd, 0);
    }
}

static void ui_apply_volume_and_mute(void)
{
    if (lvgl_port_lock(200))
    {
        ui_update_volume(*s_cfg.volume);
        ui_update_mute(*s_cfg.mute);
        lvgl_port_unlock();
    }
}

static void volume_autohide_timer_cb(void *arg)
{
    (void)arg;

    post_input_event(INPUT_EVT_AUTO_HIDE_VOLUME);
}

static void volume_activity(void)
{
    s_last_volume_activity_us = esp_timer_get_time();

    if (s_volume_autohide_timer != NULL)
    {
        esp_timer_stop(s_volume_autohide_timer);
        esp_timer_start_once(s_volume_autohide_timer, VOLUME_AUTO_HIDE_US);
    }
}

static void enter_volume_mode(void)
{
    s_ui_mode = INPUT_UI_MODE_VOLUME;

    if (lvgl_port_lock(500))
    {
        ui_show_volume_mode(true);
        ui_update_volume(*s_cfg.volume);
        ui_update_mute(*s_cfg.mute);
        lvgl_port_unlock();
    }

    volume_activity();
}

static void exit_volume_mode(void)
{
    if (s_ui_mode != INPUT_UI_MODE_VOLUME)
    {
        return;
    }

    s_ui_mode = INPUT_UI_MODE_TRACK;

    if (s_volume_autohide_timer != NULL)
    {
        esp_timer_stop(s_volume_autohide_timer);
    }

    if (lvgl_port_lock(500))
    {
        ui_show_volume_mode(false);
        lvgl_port_unlock();
    }
}

static void change_volume(int delta)
{
    int v = (int)(*s_cfg.volume) + delta;

    if (v < 0)
    {
        v = 0;
    }

    if (v > 100)
    {
        v = 100;
    }

    *s_cfg.volume = (uint8_t)v;

    /*
     * Optional UX choice:
     * If the user rotates while muted, assume they want audible volume again.
     * Remove this if you want mute to remain sticky.
     */
    if (*s_cfg.mute)
    {
        *s_cfg.mute = false;
    }

    audio_player_set_volume(*s_cfg.volume, *s_cfg.mute);
    ui_apply_volume_and_mute();
}

static void toggle_mute(void)
{
    *s_cfg.mute = !(*s_cfg.mute);

    audio_player_set_volume(*s_cfg.volume, *s_cfg.mute);
    ui_apply_volume_and_mute();
}

/* --------------------------------------------------------------------------
 * Input control task
 * -------------------------------------------------------------------------- */
static void input_control_task(void *arg)
{
    (void)arg;

    input_event_t evt;
    int64_t last_long_press_us = 0;

    while (1)
    {
        if (xQueueReceive(s_input_event_queue, &evt, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        int64_t now = esp_timer_get_time();

        if (evt == INPUT_EVT_LONG_PRESS)
        {
            last_long_press_us = now;
        }

        /*
         * Some knob/button stacks can emit a short-press event after a long
         * press release. Ignore short presses that occur immediately after a
         * long press.
         */
        if (evt == INPUT_EVT_SHORT_PRESS &&
            (now - last_long_press_us) < LONG_PRESS_GUARD_US)
        {
            continue;
        }

        /* ------------------------------------------------------------ */
        /* Top-level application mode handling                          */
        /* ------------------------------------------------------------ */
        if (*s_cfg.app_mode != APP_MODE_PLAYER)
        {
            if (*s_cfg.app_mode == APP_MODE_BOOT_MENU)
            {
                switch (evt)
                {
                case INPUT_EVT_ROTATE_CW:
                case INPUT_EVT_ROTATE_CCW:
                    *s_cfg.boot_option =
                        (*s_cfg.boot_option == BOOT_OPT_PLAYER)
                            ? BOOT_OPT_UPLOAD
                            : BOOT_OPT_PLAYER;

                    if (lvgl_port_lock(200))
                    {
                        ui_set_boot_selection(*s_cfg.boot_option);
                        lvgl_port_unlock();
                    }
                    break;

                case INPUT_EVT_SHORT_PRESS:
                    *s_cfg.app_mode = APP_MODE_BUSY;

                    if (*s_cfg.boot_option == BOOT_OPT_PLAYER)
                    {
                        post_app_cmd(APP_CMD_START_PLAYER);
                    }
                    else
                    {
                        post_app_cmd(APP_CMD_START_TRANSFER);
                    }
                    break;

                default:
                    break;
                }
            }
            else if (*s_cfg.app_mode == APP_MODE_TRANSFER)
            {
                /*
                 * In transfer mode:
                 * long press means "done, go to player".
                 */
                if (evt == INPUT_EVT_LONG_PRESS)
                {
                    *s_cfg.app_mode = APP_MODE_BUSY;
                    post_app_cmd(APP_CMD_FINISH_TRANSFER);
                }
            }

            /*
             * APP_MODE_BUSY ignores all input.
             * Also ignore rotation/short press in transfer mode.
             */
            continue;
        }

        /* ------------------------------------------------------------ */
        /* Player mode                                                  */
        /* ------------------------------------------------------------ */
        if (s_ui_mode == INPUT_UI_MODE_TRACK)
        {
            switch (evt)
            {
            case INPUT_EVT_ROTATE_CW:
                ESP_LOGI(TAG, "Encoder: next track");
                send_player_cmd(PLAYER_CMD_NEXT);
                break;

            case INPUT_EVT_ROTATE_CCW:
                ESP_LOGI(TAG, "Encoder: previous track");
                send_player_cmd(PLAYER_CMD_PREV);
                break;

            case INPUT_EVT_SHORT_PRESS:
                ESP_LOGI(TAG, "Encoder: play/pause");
                send_player_cmd(PLAYER_CMD_TOGGLE_PLAY);
                break;

            case INPUT_EVT_LONG_PRESS:
                ESP_LOGI(TAG, "Encoder: enter volume mode");
                enter_volume_mode();
                break;

            default:
                break;
            }
        }
        else /* INPUT_UI_MODE_VOLUME */
        {
            switch (evt)
            {
            case INPUT_EVT_ROTATE_CW:
                ESP_LOGI(TAG, "Encoder: volume up");
                change_volume(VOLUME_STEP);
                volume_activity();
                break;

            case INPUT_EVT_ROTATE_CCW:
                ESP_LOGI(TAG, "Encoder: volume down");
                change_volume(-VOLUME_STEP);
                volume_activity();
                break;

            case INPUT_EVT_SHORT_PRESS:
                ESP_LOGI(TAG, "Encoder: mute/unmute");
                toggle_mute();

                /*
                 * If you want auto-hide to reset only on rotation,
                 * remove this call.
                 */
                volume_activity();
                break;

            case INPUT_EVT_LONG_PRESS:
                ESP_LOGI(TAG, "Encoder: exit volume mode");
                exit_volume_mode();
                break;

            case INPUT_EVT_AUTO_HIDE_VOLUME:
                /*
                 * Guard against stale timer events: only exit if there
                 * really has been no recent volume activity.
                 */
                if ((now - s_last_volume_activity_us) >= VOLUME_AUTO_HIDE_US)
                {
                    ESP_LOGI(TAG, "Encoder: auto-hide volume mode");
                    exit_volume_mode();
                }
                break;

            default:
                break;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Knob callbacks
 * -------------------------------------------------------------------------- */
static void knob_left_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    /*
     * KNOB_LEFT is usually counter-clockwise.
     * If your encoder direction is reversed, either swap these two callbacks
     * or swap encoder A/B pins.
     */
    post_input_event(INPUT_EVT_ROTATE_CCW);
}

static void knob_right_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    post_input_event(INPUT_EVT_ROTATE_CW);
}

static void knob_short_press_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    post_input_event(INPUT_EVT_SHORT_PRESS);
}

static void knob_long_press_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    post_input_event(INPUT_EVT_LONG_PRESS);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
esp_err_t input_knob_init(const input_knob_config_t *cfg)
{
    if (cfg == NULL ||
        cfg->player_cmd_queue == NULL ||
        cfg->app_cmd_queue == NULL ||
        cfg->app_mode == NULL ||
        cfg->boot_option == NULL ||
        cfg->volume == NULL ||
        cfg->mute == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;

    s_input_event_queue = xQueueCreate(32, sizeof(input_event_t));

    if (s_input_event_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create input event queue");
        return ESP_ERR_NO_MEM;
    }

    /* Auto-hide timer for Volume Mode */
    const esp_timer_create_args_t timer_args = {
        .callback = volume_autohide_timer_cb,
        .name = "volume_autohide",
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_volume_autohide_timer));

    /* 1. Initialize Knob for Rotation (CW/CCW) */
    knob_config_t knob_cfg = {
        .gpio_encoder_a = ENCODER_PIN_A,
        .gpio_encoder_b = ENCODER_PIN_B,
        .default_direction = 0,
    };

    knob_handle_t knob = iot_knob_create(&knob_cfg);

    if (knob == NULL)
    {
        ESP_LOGE(TAG, "Failed to create knob");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL));
    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL));

    /* 2. Initialize Button for Encoder Switch (Short/Long Press) */
    const button_config_t btn_cfg = {
        .long_press_time = 0,
        .short_press_time = 0,
    };

    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = ENCODER_PIN_SW,
        .active_level = 0, /* Set to 1 if your encoder switch is active-high */
    };

    button_handle_t btn;

    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &btn));

    /* Register short press and long press callbacks */
    ESP_ERROR_CHECK(iot_button_register_cb(
        btn,
        BUTTON_SINGLE_CLICK,
        NULL,
        knob_short_press_cb,
        NULL));

    ESP_ERROR_CHECK(iot_button_register_cb(
        btn,
        BUTTON_LONG_PRESS_START,
        NULL,
        knob_long_press_cb,
        NULL));

    xTaskCreate(
        input_control_task,
        "input_ctrl",
        6144,
        NULL,
        6,
        NULL);

    ESP_LOGI(TAG, "Encoder rotation and button input initialized");

    return ESP_OK;
}