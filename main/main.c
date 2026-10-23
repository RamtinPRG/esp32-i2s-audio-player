/* main/main.c */

#include "app_controller.h"
#include "bsp_display.h"
#include "bsp_sdcard.h"
#include "input_knob.h"
#include "ui.h"
#include "ui_presenter.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* Initialize display and LVGL UI */
    bsp_display_init();
    ESP_ERROR_CHECK(ui_presenter_init());

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
    ESP_ERROR_CHECK(input_knob_init());

    ESP_LOGI(TAG, "Boot menu active");
}