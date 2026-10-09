/* main/main.c */

#include "app_mode.h"
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
    ESP_ERROR_CHECK(app_mode_start());

    /* Initialize encoder/input state machine */
    input_knob_config_t input_cfg = {
        .player_cmd_queue = app_mode_get_player_cmd_queue(),
        .app_cmd_queue = app_mode_get_app_cmd_queue(),
        .app_mode = app_mode_get_mode_ptr(),
        .boot_option = app_mode_get_boot_option_ptr(),
        .volume = app_mode_get_volume_ptr(),
        .mute = app_mode_get_mute_ptr(),
    };

    ESP_ERROR_CHECK(input_knob_init(&input_cfg));

    /* Show boot menu */
    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(true);
        ui_set_boot_selection(*app_mode_get_boot_option_ptr());
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Boot menu active");
}