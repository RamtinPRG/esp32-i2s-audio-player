/* main/app_mode.h */
#ifndef APP_MODE_H
#define APP_MODE_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ui.h"

esp_err_t app_mode_start(void);

QueueHandle_t app_mode_get_player_cmd_queue(void);
QueueHandle_t app_mode_get_app_cmd_queue(void);

volatile app_mode_t *app_mode_get_mode_ptr(void);
boot_option_t *app_mode_get_boot_option_ptr(void);

volatile uint8_t *app_mode_get_volume_ptr(void);
volatile bool *app_mode_get_mute_ptr(void);

#endif