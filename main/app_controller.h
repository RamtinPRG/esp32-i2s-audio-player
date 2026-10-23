/* main/app_mode.h */
#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ui.h"

esp_err_t app_controller_start(void);

QueueHandle_t app_controller_get_player_cmd_queue(void);
app_mode_t app_mode_get(void);

/* Task-context only. Success means queued, not completed. */
esp_err_t app_mode_submit(app_cmd_t cmd);
boot_option_t *app_controller_get_boot_option_ptr(void);

volatile uint8_t *app_controller_get_volume_ptr(void);
volatile bool *app_controller_get_mute_ptr(void);

#endif