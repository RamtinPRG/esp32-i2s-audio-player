idf_component_register(SRCS "cover_cache.c" "ui.c" "main.c"
                    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_partition driver vfs fatfs sdmmc heap log esp_driver_spi lvgl esp_lvgl_port esp_lcd
                    INCLUDE_DIRS ".")

# spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)