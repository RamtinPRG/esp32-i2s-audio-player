/* main/input_knob.h */
#ifndef INPUT_KNOB_H
#define INPUT_KNOB_H

#include <stdbool.h>
#include <stdint.h>
#include "app_types.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct
{
    QueueHandle_t player_cmd_queue;

    volatile uint8_t *volume;
    volatile bool *mute;
} input_knob_config_t;

esp_err_t input_knob_init(const input_knob_config_t *cfg);

#endif