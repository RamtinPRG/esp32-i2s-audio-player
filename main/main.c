/* main/main.c */

#include "app_controller.h"
#include "bsp_display.h"
#include "bsp_sdcard.h"
#include "input_knob.h"
#include "ui.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* Initialize display and LVGL UI */
    bsp_display_init();

    /* Mount SD card */
    if (lvgl_port_lock(500))
    {
        ui_set_status("Mounting SD card...");
        lvgl_port_unlock();
    }

    ESP_ERROR_CHECK(bsp_sdcard_mount());

    /* Start application mode control */
    ESP_ERROR_CHECK(app_controller_start());

    /* Initialize encoder/input state machine */
    input_knob_config_t input_cfg = {
        .player_cmd_queue = app_controller_get_player_cmd_queue(),
        .volume = app_controller_get_volume_ptr(),
        .mute = app_controller_get_mute_ptr(),
    };

    ESP_ERROR_CHECK(input_knob_init(&input_cfg));

    ESP_LOGI(TAG, "Boot menu active");
}