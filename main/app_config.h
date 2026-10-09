/* main/app_config.h */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* I2S pins */
#define I2S_PIN_BCLK 38
#define I2S_PIN_WS 40
#define I2S_PIN_DOUT 39

/* SD card SPI pins */
#define SD_PIN_MOSI 5
#define SD_PIN_MISO 4
#define SD_PIN_CLK 6
#define SD_PIN_CS 7
#define SD_MOUNT_POINT "/sdcard"

/* Wi-Fi Transfer Mode settings */
#define WIFI_AP_SSID "ESP-AudioPlayer"
#define WIFI_AP_PASSWORD "audio1234"
#define WIFI_AP_CHANNEL 6
#define WIFI_TRANSFER_URL "http://192.168.4.1"

/* Rotary encoder pins */
#define ENCODER_PIN_A 16
#define ENCODER_PIN_B 17
#define ENCODER_PIN_SW 18

/* Encoder behavior */
#define VOLUME_STEP 1
#define VOLUME_AUTO_HIDE_US (3000000LL) /* 3 seconds */
#define LONG_PRESS_GUARD_US (500000LL)  /* suppress short press after long press */

/* Audio buffer settings */
#define AUDIO_BUFF_SIZE 4096
#define AUDIO_BUFFER_COUNT 6

/* Playlist settings */
#define MAX_FILES 128
#define MAX_PATH_LEN 512

/* ST7789 240x280 wiring */
#define LCD_SPI_HOST SPI3_HOST
#define LCD_PIN_SCLK 10
#define LCD_PIN_MOSI 11
#define LCD_PIN_RST 12
#define LCD_PIN_DC 13
#define LCD_PIN_CS 14
#define LCD_H_RES 240
#define LCD_V_RES 280
#define LCD_BIT_PER_PIXEL 16
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#endif