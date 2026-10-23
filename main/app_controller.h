/* main/app_mode.h */
#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"
#include "esp_err.h"
#include "ui.h"

esp_err_t app_controller_start(void);
esp_err_t app_controller_post_input(input_event_t event);

app_mode_t app_controller_get_mode(void);

/* Task-context only. Success means queued, not completed. */
esp_err_t app_controller_submit(app_cmd_t cmd);

#endif