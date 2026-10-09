/* main/input_knob.h */
#ifndef INPUT_KNOB_H
#define INPUT_KNOB_H

#include "app_types.h"
#include "ui.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct
{
    QueueHandle_t player_cmd_queue;
    QueueHandle_t app_cmd_queue;

    volatile app_mode_t *app_mode;
    boot_option_t *boot_option;

    volatile uint8_t *volume;
    volatile bool *mute;
} input_knob_config_t;

esp_err_t input_knob_init(const input_knob_config_t *cfg);

#endif