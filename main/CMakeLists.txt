idf_component_register(SRCS "wifi_transfer.c" "http_upload.c" "input_knob.c" "audio_player.c" "playlist.c" "audio_output.c" "bsp_sdcard.c" "bsp_display.c" "cover_cache.c" "ui.c" "main.c"
                    PRIV_REQUIRES
                        esp_driver_i2s
                        esp_driver_gpio
                        esp_partition
                        driver
                        vfs
                        fatfs
                        sdmmc
                        heap
                        log
                        esp_driver_spi
                        lvgl
                        esp_lvgl_port
                        esp_lcd
                        esp_timer
                        esp_wifi
                        esp_event
                        nvs_flash
                        esp_netif
                        esp_http_server
                        mdns

                    EMBED_FILES "upload.html"
                        
                    INCLUDE_DIRS ".")

# spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)