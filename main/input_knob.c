/* main/input_knob.c */
#include "input_knob.h"

#include "app_config.h"
#include "app_types.h"
#include "app_controller.h"

#include <stdint.h>

#include "button_gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "iot_knob.h"

static const char *TAG = "INPUT_KNOB";

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */
static QueueHandle_t s_input_event_queue = NULL;

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

/* --------------------------------------------------------------------------
 * Input control task
 * -------------------------------------------------------------------------- */
static void input_control_task(void *arg)
{
    (void)arg;

    input_event_t event;
    int64_t last_long_press_us = 0;

    while (1)
    {
        if (xQueueReceive(
                s_input_event_queue,
                &event,
                portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        int64_t now = esp_timer_get_time();

        if (event == INPUT_EVT_LONG_PRESS)
        {
            last_long_press_us = now;
        }

        /*
         * Suppress the short press some button drivers emit
         * immediately after a long press.
         */
        if (event == INPUT_EVT_SHORT_PRESS &&
            last_long_press_us != 0 &&
            (now - last_long_press_us) < LONG_PRESS_GUARD_US)
        {
            continue;
        }

        esp_err_t ret = app_controller_post_input(event);

        if (ret != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Failed to forward input event %d: %s",
                (int)event,
                esp_err_to_name(ret));
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
esp_err_t input_knob_init(void)
{
    s_input_event_queue = xQueueCreate(32, sizeof(input_event_t));

    if (s_input_event_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create input event queue");
        return ESP_ERR_NO_MEM;
    }

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