/*
 * ESP-IDF I2S audio player with SD Card Playlist
 *
 * Scans SD card for .pcm files and plays them in alphabetical order.
 *
 * Audio format:
 *   44100 Hz, 16-bit, stereo, raw PCM
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "iot_knob.h"
#include "iot_button.h"
#include "button_gpio.h"

#include "sdkconfig.h"

#include "ui.h"
#include "cover_cache.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

static const char *TAG = "I2S_SD";

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

static i2s_chan_handle_t tx_chan;
static sdmmc_card_t *sd_card = NULL;

static QueueHandle_t free_buffer_queue = NULL;
static QueueHandle_t audio_data_queue = NULL;

typedef struct
{
    uint8_t *data;
    size_t len;
} audio_buffer_t;

typedef struct
{
    char files[MAX_FILES][MAX_PATH_LEN];
    cover_entry_t covers[MAX_FILES];
    int count;
    int current_index;
} playlist_t;

/* --------------------------------------------------------------------------
 * Global playlist and audio system state
 * -------------------------------------------------------------------------- */
static playlist_t playlist;
static bool audio_system_initialized = false;

/* --------------------------------------------------------------------------
 * Encoder / UI mode state machine
 * -------------------------------------------------------------------------- */

typedef enum
{
    INPUT_EVT_ROTATE_CW = 0,
    INPUT_EVT_ROTATE_CCW,
    INPUT_EVT_SHORT_PRESS,
    INPUT_EVT_LONG_PRESS,
    INPUT_EVT_AUTO_HIDE_VOLUME,
} input_event_t;

typedef enum
{
    PLAYER_CMD_NEXT = 0,
    PLAYER_CMD_PREV,
    PLAYER_CMD_TOGGLE_PLAY,
} player_cmd_t;

typedef enum
{
    UI_MODE_TRACK = 0,
    UI_MODE_VOLUME,
} ui_mode_t;

typedef enum
{
    APP_MODE_BOOT_MENU = 0,
    APP_MODE_BUSY,
    APP_MODE_PLAYER,
    APP_MODE_TRANSFER,
} app_mode_t;

typedef enum
{
    APP_CMD_START_PLAYER = 0,
    APP_CMD_START_TRANSFER,
    APP_CMD_FINISH_TRANSFER,
} app_cmd_t;

static QueueHandle_t input_event_queue = NULL;
static QueueHandle_t player_cmd_queue = NULL;
static esp_timer_handle_t volume_autohide_timer = NULL;

static ui_mode_t ui_mode = UI_MODE_TRACK;

static QueueHandle_t app_cmd_queue = NULL;
static volatile app_mode_t app_mode = APP_MODE_BOOT_MENU;
static boot_option_t boot_option = BOOT_OPT_PLAYER;

static volatile uint8_t app_volume = 100;
static volatile bool app_mute = false;
static volatile bool app_playing = true;

static int64_t last_volume_activity_us = 0;

/* --------------------------------------------------------------------------
 * Wi-Fi transfer mode state
 * -------------------------------------------------------------------------- */
static bool wifi_tcpip_initialized = false;
static bool wifi_driver_initialized = false;
static bool wifi_ap_running = false;
static esp_netif_t *wifi_ap_netif = NULL;
static esp_event_handler_instance_t wifi_event_instance = NULL;
static httpd_handle_t upload_server = NULL;

/* --------------------------------------------------------------------------
 * Embedded HTML Web Page for File Upload
 * -------------------------------------------------------------------------- */
static const char UPLOAD_HTML[] =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<meta charset='utf-8'>\n"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
    "<title>ESP-AudioPlayer</title>\n"
    "<style>\n"
    "body{font-family:sans-serif;background:#08090C;color:#F2F6FF;text-align:center;padding:20px;}\n"
    "h1{color:#00D5FF;}\n"
    ".card{background:#121620;border:1px solid #263143;border-radius:12px;padding:20px;margin:20px auto;max-width:400px;}\n"
    "input[type=file]{margin:10px 0;color:#F2F6FF;}\n"
    "button{background:#00D5FF;color:#000;border:none;padding:10px 20px;border-radius:8px;font-size:16px;cursor:pointer;}\n"
    "button:disabled{background:#8A93A6;cursor:not-allowed;}\n"
    "#status{margin-top:15px;font-weight:bold;min-height:20px;}\n"
    ".progress{width:100%;background:#263143;border-radius:4px;overflow:hidden;margin-top:10px;display:none;}\n"
    ".progress-bar{height:8px;background:#00D5FF;width:0%;transition:width 0.2s;}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>Wi-Fi Upload</h1>\n"
    "<div class='card'>\n"
    "<p>Select <b>.pcm</b> or <b>.bmp</b> files.</p>\n"
    "<input type='file' id='fileInput' multiple accept='.pcm,.bmp'><br>\n"
    "<button id='uploadBtn' onclick='uploadFiles()'>Upload</button>\n"
    "<div class='progress' id='progressContainer'><div class='progress-bar' id='progressBar'></div></div>\n"
    "<div id='status'></div>\n"
    "</div>\n"
    "<script>\n"
    "async function uploadFiles(){\n"
    "  const files=document.getElementById('fileInput').files;\n"
    "  const status=document.getElementById('status');\n"
    "  const btn=document.getElementById('uploadBtn');\n"
    "  const progContainer=document.getElementById('progressContainer');\n"
    "  const progBar=document.getElementById('progressBar');\n"
    "  if(files.length===0){status.textContent='Please select files.';return;}\n"
    "  btn.disabled=true;progContainer.style.display='block';\n"
    "  for(let i=0;i<files.length;i++){\n"
    "    const file=files[i];\n"
    "    status.textContent='Uploading '+file.name+' ('+(i+1)+'/'+files.length+')...';\n"
    "    progBar.style.width='0%';\n"
    "    try{\n"
    "      await uploadSingleFile(file,progBar);\n"
    "      status.textContent=file.name+' OK!';\n"
    "    }catch(e){\n"
    "      status.textContent='Error: '+e.message;\n"
    "      btn.disabled=false;\n"
    "      return;\n"
    "    }\n"
    "  }\n"
    "  status.textContent='All files uploaded!';\n"
    "  btn.disabled=false;\n"
    "  progBar.style.width='100%';\n"
    "}\n"
    "function uploadSingleFile(file,progBar){\n"
    "  return new Promise((resolve,reject)=>{\n"
    "    const xhr=new XMLHttpRequest();\n"
    "    const url='/upload?filename='+encodeURIComponent(file.name);\n"
    "    xhr.open('POST',url,true);\n"
    "    xhr.setRequestHeader('Content-Type','application/octet-stream');\n"
    "    xhr.upload.onprogress=(e)=>{\n"
    "      if(e.lengthComputable){progBar.style.width=((e.loaded/e.total)*100)+'%';}\n"
    "    };\n"
    "    xhr.onload=()=>{\n"
    "      if(xhr.status===200){resolve();}\n"
    "      else{reject(new Error(xhr.responseText||'Server error'));}\n"
    "    };\n"
    "    xhr.onerror=()=>reject(new Error('Network error'));\n"
    "    xhr.send(file);\n"
    "  });\n"
    "}\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

/* --------------------------------------------------------------------------
 * HTTP Handlers
 * -------------------------------------------------------------------------- */

static bool is_valid_filename(const char *name)
{
    if (!name || name[0] == '\0')
        return false;
    size_t len = strlen(name);
    if (len < 5 || len > 64)
        return false;

    /* Must end in .pcm or .bmp */
    const char *ext = strrchr(name, '.');
    if (!ext)
        return false;
    if (strcasecmp(ext, ".pcm") != 0 && strcasecmp(ext, ".bmp") != 0)
        return false;

    /* Only allow safe alphanumeric characters, dot, dash, underscore */
    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_'))
        {
            return false;
        }
    }
    return true;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, UPLOAD_HTML, sizeof(UPLOAD_HTML) - 1);
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char query[128] = {0};
    char filename[65] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query string");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "filename", filename, sizeof(filename)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }

    if (!is_valid_filename(filename))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename. Use alphanumeric, dot, dash, underscore. Max 64 chars. Must end in .pcm or .bmp");
        return ESP_FAIL;
    }

    char tmp_path[140];
    char final_path[140];
    snprintf(tmp_path, sizeof(tmp_path), SD_MOUNT_POINT "/%s.tmp", filename);
    snprintf(final_path, sizeof(final_path), SD_MOUNT_POINT "/%s", filename);

    ESP_LOGI(TAG, "Upload start: %s", final_path);

    if (lvgl_port_lock(100))
    {
        ui_set_transfer_status("Uploading...");
        lvgl_port_unlock();
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file on SD");
        return ESP_FAIL;
    }

    size_t remaining = req->content_len;
    size_t total_written = 0;
    static char rx_buffer[4096]; /* static to save stack */
    bool failed = false;

    while (remaining > 0)
    {
        size_t to_read = remaining < sizeof(rx_buffer) ? remaining : sizeof(rx_buffer);
        int received = httpd_req_recv(req, rx_buffer, to_read);

        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue; /* Retry on timeout */
            }
            ESP_LOGE(TAG, "Upload receive failed");
            failed = true;
            break;
        }

        size_t written = fwrite(rx_buffer, 1, received, f);
        if (written != (size_t)received)
        {
            ESP_LOGE(TAG, "SD write failed");
            failed = true;
            break;
        }

        remaining -= received;
        total_written += written;

        /* Update UI progress occasionally */
        if ((total_written % (64 * 1024)) < sizeof(rx_buffer))
        {
            if (lvgl_port_lock(10))
            {
                ui_update_transfer_progress(total_written, req->content_len);
                lvgl_port_unlock();
            }
        }
    }

    fclose(f);

    if (failed)
    {
        remove(tmp_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        return ESP_FAIL;
    }

    remove(final_path); /* Replace if exists */
    if (rename(tmp_path, final_path) != 0)
    {
        remove(tmp_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Upload complete: %s (%u bytes)", final_path, (unsigned)total_written);

    if (lvgl_port_lock(100))
    {
        ui_set_transfer_status("Upload complete!");
        ui_update_transfer_progress(100, 100);
        lvgl_port_unlock();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * HTTP Server Lifecycle
 * -------------------------------------------------------------------------- */
static void http_upload_server_start(void)
{
    if (upload_server != NULL)
    {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.recv_wait_timeout = 30; /* seconds */
    config.send_wait_timeout = 30; /* seconds */

    ESP_ERROR_CHECK(httpd_start(&upload_server, &config));

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(upload_server, &root_uri);

    httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
    };
    httpd_register_uri_handler(upload_server, &upload_uri);

    ESP_LOGI(TAG, "HTTP upload server started");
}

static void http_upload_server_stop(void)
{
    if (upload_server != NULL)
    {
        httpd_stop(upload_server);
        upload_server = NULL;
        ESP_LOGI(TAG, "HTTP upload server stopped");
    }
}

/* --------------------------------------------------------------------------
 * Wi-Fi event handler
 * -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != WIFI_EVENT)
    {
        return;
    }

    switch (event_id)
    {
    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "Wi-Fi AP started");
        break;

    case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "Wi-Fi AP stopped");
        break;

    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Wi-Fi station connected");
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "Wi-Fi station disconnected");
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * Start Wi-Fi Access Point for transfer mode.
 *
 * This function is re-entrant:
 * - first call initializes Wi-Fi
 * - later calls only start Wi-Fi again
 * -------------------------------------------------------------------------- */
static void wifi_transfer_start(void)
{
    esp_err_t ret;

    if (!wifi_tcpip_initialized)
    {
        ESP_LOGI(TAG, "Initializing NVS / netif / event loop for Wi-Fi");

        /* Initialize NVS */
        ret = nvs_flash_init();

        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }

        ESP_ERROR_CHECK(ret);

        /* Initialize TCP/IP and event loop */
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        wifi_tcpip_initialized = true;
    }

    if (!wifi_driver_initialized)
    {
        ESP_LOGI(TAG, "Creating Wi-Fi AP interface");

        /* Create default Wi-Fi AP network interface */
        wifi_ap_netif = esp_netif_create_default_wifi_ap();
        assert(wifi_ap_netif != NULL);

        /* Configure static IP: 192.168.4.1 */
        esp_netif_ip_info_t ip_info;

        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

        /* Ignore result here: DHCP server may already be stopped */
        esp_netif_dhcps_stop(wifi_ap_netif);

        ESP_ERROR_CHECK(esp_netif_set_ip_info(wifi_ap_netif, &ip_info));
        ESP_ERROR_CHECK(esp_netif_dhcps_start(wifi_ap_netif));

        /* Initialize Wi-Fi */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        /* Register Wi-Fi event handler */
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &wifi_event_instance));

        /* AP configuration */
        wifi_config_t wifi_config = {0};

        snprintf((char *)wifi_config.ap.ssid,
                 sizeof(wifi_config.ap.ssid),
                 "%s",
                 WIFI_AP_SSID);

        snprintf((char *)wifi_config.ap.password,
                 sizeof(wifi_config.ap.password),
                 "%s",
                 WIFI_AP_PASSWORD);

        wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
        wifi_config.ap.channel = WIFI_AP_CHANNEL;

        /*
         * Use only one station.
         * This reduces Wi-Fi memory usage and is enough for file upload.
         */
        wifi_config.ap.max_connection = 1;
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

        wifi_driver_initialized = true;
    }
    else
    {
        /*
         * If the driver already exists but was only stopped, make sure the
         * DHCP server is running again.
         */
        if (wifi_ap_netif != NULL)
        {
            esp_netif_dhcps_start(wifi_ap_netif);
        }
    }

    if (!wifi_ap_running)
    {
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_ap_running = true;

        ESP_LOGI(TAG, "Wi-Fi transfer AP active: SSID=%s, URL=%s",
                 WIFI_AP_SSID, WIFI_TRANSFER_URL);
    }
}

/* --------------------------------------------------------------------------
 * Stop and fully deinitialize Wi-Fi before entering Player mode.
 *
 * This frees Wi-Fi resources so playback, LVGL, and cover loading have more
 * memory available.
 * -------------------------------------------------------------------------- */
static void wifi_transfer_stop(void)
{
    if (!wifi_driver_initialized)
    {
        return;
    }

    if (wifi_ap_running)
    {
        ESP_LOGI(TAG, "Stopping Wi-Fi transfer AP");

        if (wifi_ap_netif != NULL)
        {
            esp_netif_dhcps_stop(wifi_ap_netif);
        }

        esp_err_t ret = esp_wifi_stop();

        if (ret == ESP_OK)
        {
            wifi_ap_running = false;
        }
        else
        {
            ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
        }

        /* Give Wi-Fi a little time to finish disconnect/cleanup */
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    /* Unregister event handler */
    if (wifi_event_instance != NULL)
    {
        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_instance);

        wifi_event_instance = NULL;
    }

    /* Fully deinitialize Wi-Fi driver */
    esp_err_t ret = esp_wifi_deinit();

    if (ret == ESP_OK)
    {
        wifi_driver_initialized = false;

        if (wifi_ap_netif != NULL)
        {
            esp_netif_destroy_default_wifi(wifi_ap_netif);
            wifi_ap_netif = NULL;
        }

        ESP_LOGI(TAG, "Wi-Fi fully deinitialized");
    }
    else
    {
        ESP_LOGW(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(ret));
    }
}

/* --------------------------------------------------------------------------
 * Encoder input helpers
 * -------------------------------------------------------------------------- */

static void post_input_event(input_event_t evt)
{
    if (input_event_queue == NULL)
    {
        return;
    }

    if (xPortInIsrContext() == pdTRUE)
    {
        BaseType_t higher_woken = pdFALSE;
        xQueueSendFromISR(input_event_queue, &evt, &higher_woken);
        if (higher_woken)
        {
            portYIELD_FROM_ISR();
        }
    }
    else
    {
        xQueueSend(input_event_queue, &evt, 0);
    }
}

static void send_player_cmd(player_cmd_t cmd)
{
    if (player_cmd_queue == NULL)
    {
        return;
    }

    /*
     * Use zero timeout so the input task never blocks.
     * If the queue is full, dropping one encoder command is usually preferable
     * to blocking the UI/input task.
     */
    xQueueSend(player_cmd_queue, &cmd, 0);
}

static void post_app_cmd(app_cmd_t cmd)
{
    if (app_cmd_queue == NULL)
    {
        return;
    }

    if (xPortInIsrContext() == pdTRUE)
    {
        BaseType_t higher_woken = pdFALSE;
        xQueueSendFromISR(app_cmd_queue, &cmd, &higher_woken);
        if (higher_woken)
        {
            portYIELD_FROM_ISR();
        }
    }
    else
    {
        xQueueSend(app_cmd_queue, &cmd, 0);
    }
}

static void ui_apply_volume_and_mute(void)
{
    if (lvgl_port_lock(200))
    {
        ui_update_volume(app_volume);
        ui_update_mute(app_mute);
        lvgl_port_unlock();
    }
}

static void volume_autohide_timer_cb(void *arg)
{
    (void)arg;
    post_input_event(INPUT_EVT_AUTO_HIDE_VOLUME);
}

static void volume_activity(void)
{
    last_volume_activity_us = esp_timer_get_time();

    if (volume_autohide_timer != NULL)
    {
        esp_timer_stop(volume_autohide_timer);
        esp_timer_start_once(volume_autohide_timer, VOLUME_AUTO_HIDE_US);
    }
}

static void enter_volume_mode(void)
{
    ui_mode = UI_MODE_VOLUME;

    if (lvgl_port_lock(500))
    {
        ui_show_volume_mode(true);
        ui_update_volume(app_volume);
        ui_update_mute(app_mute);
        lvgl_port_unlock();
    }

    volume_activity();
}

static void exit_volume_mode(void)
{
    if (ui_mode != UI_MODE_VOLUME)
    {
        return;
    }

    ui_mode = UI_MODE_TRACK;

    if (volume_autohide_timer != NULL)
    {
        esp_timer_stop(volume_autohide_timer);
    }

    if (lvgl_port_lock(500))
    {
        ui_show_volume_mode(false);
        lvgl_port_unlock();
    }
}

static void change_volume(int delta)
{
    int v = (int)app_volume + delta;

    if (v < 0)
    {
        v = 0;
    }

    if (v > 100)
    {
        v = 100;
    }

    app_volume = (uint8_t)v;

    /*
     * Optional UX choice:
     * If the user rotates while muted, assume they want audible volume again.
     * Remove this if you want mute to remain sticky.
     */
    if (app_mute)
    {
        app_mute = false;
    }

    ui_apply_volume_and_mute();
}

static void toggle_mute(void)
{
    app_mute = !app_mute;
    ui_apply_volume_and_mute();
}

/* --------------------------------------------------------------------------
 * Input control task
 * -------------------------------------------------------------------------- */

static void input_control_task(void *arg)
{
    (void)arg;

    input_event_t evt;
    int64_t last_long_press_us = 0;

    while (1)
    {
        if (xQueueReceive(input_event_queue, &evt, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        int64_t now = esp_timer_get_time();

        if (evt == INPUT_EVT_LONG_PRESS)
        {
            last_long_press_us = now;
        }

        /*
         * Some knob/button stacks can emit a short-press event after a long press
         * release. Ignore short presses that occur immediately after a long press.
         */
        if (evt == INPUT_EVT_SHORT_PRESS &&
            (now - last_long_press_us) < LONG_PRESS_GUARD_US)
        {
            continue;
        }

        /* ------------------------------------------------------------ */
        /* Top-level application mode handling                          */
        /* ------------------------------------------------------------ */
        if (app_mode != APP_MODE_PLAYER)
        {
            if (app_mode == APP_MODE_BOOT_MENU)
            {
                switch (evt)
                {
                case INPUT_EVT_ROTATE_CW:
                case INPUT_EVT_ROTATE_CCW:
                    boot_option = (boot_option == BOOT_OPT_PLAYER)
                                      ? BOOT_OPT_UPLOAD
                                      : BOOT_OPT_PLAYER;

                    if (lvgl_port_lock(200))
                    {
                        ui_set_boot_selection(boot_option);
                        lvgl_port_unlock();
                    }
                    break;

                case INPUT_EVT_SHORT_PRESS:
                    app_mode = APP_MODE_BUSY;

                    if (boot_option == BOOT_OPT_PLAYER)
                    {
                        post_app_cmd(APP_CMD_START_PLAYER);
                    }
                    else
                    {
                        post_app_cmd(APP_CMD_START_TRANSFER);
                    }
                    break;

                default:
                    break;
                }
            }
            else if (app_mode == APP_MODE_TRANSFER)
            {
                /*
                 * In transfer mode:
                 * long press means "done, go to player".
                 */
                if (evt == INPUT_EVT_LONG_PRESS)
                {
                    app_mode = APP_MODE_BUSY;
                    post_app_cmd(APP_CMD_FINISH_TRANSFER);
                }
            }

            /*
             * APP_MODE_BUSY ignores all input.
             * Also ignore rotation/short press in transfer mode.
             */
            continue;
        }

        if (ui_mode == UI_MODE_TRACK)
        {
            switch (evt)
            {
            case INPUT_EVT_ROTATE_CW:
                ESP_LOGI(TAG, "Encoder: next track");
                send_player_cmd(PLAYER_CMD_NEXT);
                break;

            case INPUT_EVT_ROTATE_CCW:
                ESP_LOGI(TAG, "Encoder: previous track");
                send_player_cmd(PLAYER_CMD_PREV);
                break;

            case INPUT_EVT_SHORT_PRESS:
                ESP_LOGI(TAG, "Encoder: play/pause");
                send_player_cmd(PLAYER_CMD_TOGGLE_PLAY);
                break;

            case INPUT_EVT_LONG_PRESS:
                ESP_LOGI(TAG, "Encoder: enter volume mode");
                enter_volume_mode();
                break;

            default:
                break;
            }
        }
        else /* UI_MODE_VOLUME */
        {
            switch (evt)
            {
            case INPUT_EVT_ROTATE_CW:
                ESP_LOGI(TAG, "Encoder: volume up");
                change_volume(VOLUME_STEP);
                volume_activity();
                break;

            case INPUT_EVT_ROTATE_CCW:
                ESP_LOGI(TAG, "Encoder: volume down");
                change_volume(-VOLUME_STEP);
                volume_activity();
                break;

            case INPUT_EVT_SHORT_PRESS:
                ESP_LOGI(TAG, "Encoder: mute/unmute");
                toggle_mute();

                /*
                 * If you want auto-hide to reset only on rotation,
                 * remove this call.
                 */
                volume_activity();
                break;

            case INPUT_EVT_LONG_PRESS:
                ESP_LOGI(TAG, "Encoder: exit volume mode");
                exit_volume_mode();
                break;

            case INPUT_EVT_AUTO_HIDE_VOLUME:
                /*
                 * Guard against stale timer events: only exit if there really
                 * has been no recent volume activity.
                 */
                if ((now - last_volume_activity_us) >= VOLUME_AUTO_HIDE_US)
                {
                    ESP_LOGI(TAG, "Encoder: auto-hide volume mode");
                    exit_volume_mode();
                }
                break;

            default:
                break;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Knob callbacks
 * -------------------------------------------------------------------------- */

static void knob_left_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    /*
     * KNOB_LEFT is usually counter-clockwise.
     * If your encoder direction is reversed, either swap these two callbacks
     * or swap encoder A/B pins.
     */
    post_input_event(INPUT_EVT_ROTATE_CCW);
}

static void knob_right_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    post_input_event(INPUT_EVT_ROTATE_CW);
}

static void knob_short_press_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    post_input_event(INPUT_EVT_SHORT_PRESS);
}

static void knob_long_press_cb(void *knob_handle, void *user_data)
{
    (void)knob_handle;
    (void)user_data;

    post_input_event(INPUT_EVT_LONG_PRESS);
}

/* --------------------------------------------------------------------------
Encoder and Button init
-------------------------------------------------------------------------- */
static void encoder_input_init(void)
{
    /* Auto-hide timer for Volume Mode */
    const esp_timer_create_args_t timer_args = {
        .callback = volume_autohide_timer_cb,
        .name = "volume_autohide",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &volume_autohide_timer));

    /* 1. Initialize Knob for Rotation (CW/CCW) */
    knob_config_t knob_cfg = {
        .gpio_encoder_a = ENCODER_PIN_A,
        .gpio_encoder_b = ENCODER_PIN_B,
        .default_direction = 0,
    };
    knob_handle_t knob = iot_knob_create(&knob_cfg);
    ESP_ERROR_CHECK(knob == NULL ? ESP_FAIL : ESP_OK);

    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL));
    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL));

    /* 2. Initialize Button for Encoder Switch (Short/Long Press) */
    const button_config_t btn_cfg = {
        .long_press_time = 0,  /* 1.5s for long press */
        .short_press_time = 0, /* 0.8s for short press */
    };
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = ENCODER_PIN_SW,
        .active_level = 0, /* Set to 1 if your encoder switch is active-high */
    };
    button_handle_t btn;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &btn));

    /* Register short press (single click) and long press callbacks */
    /* Note: passing NULL for event_args uses the default times defined in btn_cfg */
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, knob_short_press_cb, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, knob_long_press_cb, NULL));

    xTaskCreate(input_control_task, "input_ctrl", 6144, NULL, 6, NULL);
    ESP_LOGI(TAG, "Encoder rotation and button input initialized");
}

/* --------------------------------------------------------------------------
 * Audio control helpers
 * -------------------------------------------------------------------------- */

static void flush_audio_data_queue(void)
{
    audio_buffer_t tmp;

    /*
     * Move any queued full audio buffers back to the free queue.
     *
     * This is used for:
     * - pause
     * - next track
     * - previous track
     *
     * A few samples may still be in the I2S DMA, but this keeps the skip
     * latency low without needing to tear down the I2S channel.
     */
    while (xQueueReceive(audio_data_queue, &tmp, 0) == pdTRUE)
    {
        xQueueSend(free_buffer_queue, &tmp, pdMS_TO_TICKS(10));
    }
}

static void apply_volume_to_buffer(audio_buffer_t *buf)
{
    if (buf == NULL || buf->data == NULL || buf->len == 0)
    {
        return;
    }

    bool mute = app_mute;
    uint8_t volume = app_volume;

    if (mute || volume == 0)
    {
        memset(buf->data, 0, buf->len);
        return;
    }

    if (volume >= 100)
    {
        return;
    }

    int16_t *samples = (int16_t *)buf->data;
    size_t sample_count = buf->len / sizeof(int16_t);

    /*
     * Simple linear volume:
     *
     *     out = in * volume / 100
     *
     * If you want a more perceptual volume curve, replace this with:
     *
     *     uint32_t gain = (volume * volume) / 100;
     *
     * or use a lookup table.
     */
    int32_t gain = volume * volume / 100;

    for (size_t i = 0; i < sample_count; i++)
    {
        samples[i] = (int16_t)(((int32_t)samples[i] * gain) / 100);
    }
}

/*
 * Initialize the Display Hardware and LVGL Port
 */
static void display_hardware_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7789 + LVGL");

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 20));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .color_format = LV_COLOR_FORMAT_RGB565,
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(disp == NULL ? ESP_FAIL : ESP_OK);

    /* Lock LVGL before calling our platform-agnostic UI init */
    if (lvgl_port_lock(1000))
    {
        ui_init();
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG, "Failed to lock LVGL during UI creation");
    }
    ESP_LOGI(TAG, "LVGL UI ready");
}

/*
 * Mount SD card using SPI mode.
 */
static esp_err_t mount_sdcard(void)
{
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Initializing SD card");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AUDIO_BUFF_SIZE,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. Make sure the SD card is FAT32.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");
    sdmmc_card_print_info(stdout, sd_card);
    return ESP_OK;
}

/*
 * Initialize I2S standard mode: 44100 Hz, 16-bit, stereo.
 */
static void i2s_init_std(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCLK,
            .ws = I2S_PIN_WS,
            .dout = I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
}

static int compare_strings(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/*
 * Scans the SD card for .pcm files and builds a sorted playlist.
 */
static esp_err_t build_playlist(playlist_t *playlist)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to open directory %s", SD_MOUNT_POINT);
        return ESP_FAIL;
    }

    playlist->count = 0;
    struct dirent *entry;

    ESP_LOGI(TAG, "Scanning for .pcm files...");

    while ((entry = readdir(dir)) != NULL && playlist->count < MAX_FILES)
    {
        if (entry->d_type == DT_REG)
        {
            const char *name = entry->d_name;
            int len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".pcm") == 0)
            {
                snprintf(playlist->files[playlist->count], MAX_PATH_LEN, "%s/%s", SD_MOUNT_POINT, name);
                playlist->count++;
            }
        }
    }
    closedir(dir);

    if (playlist->count == 0)
    {
        ESP_LOGE(TAG, "No .pcm files found on SD card!");
        return ESP_ERR_NOT_FOUND;
    }

    qsort(playlist->files, playlist->count, MAX_PATH_LEN, compare_strings);

    ESP_LOGI(TAG, "Found %d PCM file(s):", playlist->count);
    for (int i = 0; i < playlist->count; i++)
    {
        ESP_LOGI(TAG, "  [%d] %s", i + 1, playlist->files[i]);
    }

    playlist->current_index = 0;
    return ESP_OK;
}

static void preload_cover_art(playlist_t *playlist)
{
    int loaded = 0;

    for (int i = 0; i < playlist->count; i++)
    {
        if (cover_cache_load_one(playlist->files[i], &playlist->covers[i]))
        {
            loaded++;
        }

        /*
         * Optional: yield briefly so long preload loops do not starve
         * lower priority tasks.
         */
        if ((i % 16) == 15)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    ESP_LOGI(TAG, "Preloaded %d/%d cover images into PSRAM",
             loaded, playlist->count);
}

/*
 * Task that reads audio data from the SD card playlist.
 */
static void sd_read_task(void *arg)
{
    playlist_t *playlist = (playlist_t *)arg;
    audio_buffer_t buf;
    FILE *f = NULL;

    uint32_t track_bytes_read = 0;
    uint32_t last_elapsed_sec = 0;

    while (1)
    {
        /* ------------------------------------------------------------
         * Handle player commands from encoder/input task
         * ------------------------------------------------------------ */
        player_cmd_t cmd;

        while (xQueueReceive(player_cmd_queue, &cmd, 0) == pdTRUE)
        {
            if (cmd == PLAYER_CMD_NEXT || cmd == PLAYER_CMD_PREV)
            {
                if (f != NULL)
                {
                    fclose(f);
                    f = NULL;
                }

                flush_audio_data_queue();

                if (cmd == PLAYER_CMD_NEXT)
                {
                    playlist->current_index =
                        (playlist->current_index + 1) % playlist->count;
                }
                else
                {
                    playlist->current_index =
                        (playlist->current_index - 1 + playlist->count) % playlist->count;
                }
            }
            else if (cmd == PLAYER_CMD_TOGGLE_PLAY)
            {
                app_playing = !app_playing;

                // if (!app_playing)
                // {
                //     flush_audio_data_queue();
                // }

                if (lvgl_port_lock(500))
                {
                    ui_update_playback(app_playing);
                    lvgl_port_unlock();
                }
            }
        }

        /* ------------------------------------------------------------
         * Open current track if needed
         * ------------------------------------------------------------ */
        if (f == NULL)
        {
            const char *filepath = playlist->files[playlist->current_index];
            ESP_LOGI(TAG, "Opening track %d/%d: %s", playlist->current_index + 1, playlist->count, filepath);

            f = fopen(filepath, "rb");
            if (f == NULL)
            {
                ESP_LOGE(TAG, "Failed to open file: %s", filepath);
                playlist->current_index = (playlist->current_index + 1) % playlist->count;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // Get file size for logging
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);

            // Calculate duration: 44100 Hz * 2 channels * 2 bytes = 176400 bytes/sec
            float duration_sec_f = size / 176400.0f;
            uint32_t duration_sec = (uint32_t)duration_sec_f;
            int mins = (int)duration_sec_f / 60;
            int secs = (int)duration_sec_f % 60;
            ESP_LOGI(TAG, "▶ Started playing: %s (%.2f MB, %02d:%02d)", filepath, size / (1024.0f * 1024.0f), mins, secs);

            track_bytes_read = 0;
            last_elapsed_sec = 0;

            // --- Check for corresponding .bmp cover art ---
            const void *img_arg = NULL;

            if (playlist->covers[playlist->current_index].valid)
            {
                img_arg = &playlist->covers[playlist->current_index].dsc;
            }

            /* Thread-safe UI update */
            if (lvgl_port_lock(500))
            {
                ui_notify_track_started(filepath, img_arg, playlist->current_index + 1, playlist->count, duration_sec);

                /*
                 * If the user changed track while paused, keep the UI in the
                 * paused state.
                 */
                ui_update_playback(app_playing);

                lvgl_port_unlock();
            }
        }

        /* ------------------------------------------------------------
         * If paused, send silence buffers to flush I2S DMA
         * ------------------------------------------------------------ */
        if (!app_playing)
        {
            if (xQueueReceive(free_buffer_queue, &buf, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                memset(buf.data, 0, AUDIO_BUFF_SIZE);
                buf.len = AUDIO_BUFF_SIZE;

                if (xQueueSend(audio_data_queue, &buf, pdMS_TO_TICKS(10)) != pdTRUE)
                {
                    // Return buffer to free queue if send timed out
                    xQueueSend(free_buffer_queue, &buf, 0);
                }
            }
            continue;
        }

        /* ------------------------------------------------------------
         * Get an empty buffer
         * ------------------------------------------------------------ */
        if (xQueueReceive(free_buffer_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* ------------------------------------------------------------
         * Read from SD card
         * ------------------------------------------------------------ */
        size_t bytes_read = fread(buf.data, 1, AUDIO_BUFF_SIZE, f);

        if (bytes_read == 0)
        {
            ESP_LOGI(TAG, "⏹ Finished playing: %s", playlist->files[playlist->current_index]);

            /* Thread-safe UI update */
            if (lvgl_port_lock(500))
            {
                ui_notify_track_finished(playlist->files[playlist->current_index]);
                lvgl_port_unlock();
            }

            fclose(f);
            f = NULL;

            /* Move to next track */
            playlist->current_index = (playlist->current_index + 1) % playlist->count;

            /* Return the empty buffer to the free queue */
            xQueueSend(free_buffer_queue, &buf, 0);

            // Small delay to let I2S DMA finish playing the last buffers smoothly
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        buf.len = bytes_read;

        track_bytes_read += bytes_read;
        uint32_t current_elapsed_sec = track_bytes_read / 176400;

        if (current_elapsed_sec != last_elapsed_sec)
        {
            /* Use a short timeout so UI locking doesn't stutter the audio DMA */
            if (lvgl_port_lock(10))
            {
                ui_update_elapsed(current_elapsed_sec);
                lvgl_port_unlock();
                last_elapsed_sec = current_elapsed_sec;
            }
        }

        /* Send filled buffer to I2S writer */
        if (xQueueSend(audio_data_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            ESP_LOGE(TAG, "Failed to send audio buffer to data queue");
        }
    }
}

/*
 * Task that writes audio data to I2S.
 */
static void i2s_write_task(void *arg)
{
    (void)arg;

    audio_buffer_t buf;

    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    while (1)
    {
        if (xQueueReceive(audio_data_queue, &buf, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        /* Apply software volume/mute */
        apply_volume_to_buffer(&buf);

        size_t offset = 0;

        while (offset < buf.len)
        {
            size_t bytes_written = 0;

            esp_err_t ret = i2s_channel_write(
                tx_chan,
                buf.data + offset,
                buf.len - offset,
                &bytes_written,
                portMAX_DELAY);

            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
                break;
            }

            offset += bytes_written;
        }

        /* Return buffer to read task */
        xQueueSend(free_buffer_queue, &buf, 0);
    }
}

/* --------------------------------------------------------------------------
 * Start the audio system lazily.
 *
 * This is called only when we actually enter Player mode.
 * -------------------------------------------------------------------------- */
static void audio_system_start(void)
{
    if (!audio_system_initialized)
    {
        ESP_LOGI(TAG, "Initializing audio system");

        /* Initialize I2S */
        i2s_init_std();

        /* Create buffer queues */
        free_buffer_queue = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_buffer_t));
        audio_data_queue = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_buffer_t));

        assert(free_buffer_queue != NULL);
        assert(audio_data_queue != NULL);

        /* Allocate DMA-capable audio buffers */
        for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
        {
            uint8_t *mem = heap_caps_calloc(
                AUDIO_BUFF_SIZE,
                1,
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

            if (mem == NULL)
            {
                ESP_LOGE(TAG, "Failed to allocate DMA-capable audio buffer");
                abort();
            }

            audio_buffer_t buf = {
                .data = mem,
                .len = 0,
            };

            xQueueSend(free_buffer_queue, &buf, 0);
        }

        /* Create audio tasks */
        xTaskCreate(sd_read_task, "sd_read_task", 8192, &playlist, 6, NULL);
        xTaskCreate(i2s_write_task, "i2s_write_task", 4096, NULL, 5, NULL);

        audio_system_initialized = true;
    }

    app_playing = true;
}

/* --------------------------------------------------------------------------
 * Enter normal Player mode.
 *
 * This scans the SD card, loads cover art, and starts audio playback.
 * If no PCM files are found, it returns to the boot menu.
 * -------------------------------------------------------------------------- */
static void start_player_mode(bool from_transfer)
{
    (void)from_transfer;

    app_mode = APP_MODE_BUSY;

    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(false);
        ui_show_transfer_screen(false, NULL, NULL, NULL);
        ui_set_status("Scanning PCM files...");
        lvgl_port_unlock();
    }

    esp_err_t ret = build_playlist(&playlist);

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "No playable PCM files found");

        if (lvgl_port_lock(500))
        {
            ui_set_status("No PCM files found!");
            lvgl_port_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(1500));

        if (lvgl_port_lock(500))
        {
            ui_show_boot_menu(true);
            ui_set_boot_selection(boot_option);
            lvgl_port_unlock();
        }

        app_mode = APP_MODE_BOOT_MENU;
        return;
    }

    if (lvgl_port_lock(500))
    {
        ui_set_status("Loading cover art...");
        lvgl_port_unlock();
    }

    preload_cover_art(&playlist);

    if (lvgl_port_lock(500))
    {
        ui_set_status("Starting playback...");
        lvgl_port_unlock();
    }

    audio_system_start();

    app_mode = APP_MODE_PLAYER;
}

/* --------------------------------------------------------------------------
 * Enter Transfer mode placeholder.
 *
 * Later this will start:
 * - Wi-Fi AP
 * - HTTP upload server
 * - transfer screen UI
 * -------------------------------------------------------------------------- */
static void enter_transfer_from_boot_menu(void)
{
    app_mode = APP_MODE_BUSY;
    app_playing = false;

    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(false);

        ui_show_transfer_screen(true,
                                WIFI_AP_SSID,
                                WIFI_AP_PASSWORD,
                                WIFI_TRANSFER_URL);

        ui_set_transfer_status("Starting Wi-Fi...");
        lvgl_port_unlock();
    }

    wifi_transfer_start();
    http_upload_server_start(); /* <-- START HTTP HERE */

    if (lvgl_port_lock(500))
    {
        ui_set_transfer_status("Wi-Fi & HTTP ready.");
        lvgl_port_unlock();
    }

    app_mode = APP_MODE_TRANSFER;
}

/* --------------------------------------------------------------------------
 * Finish Transfer mode and enter Player mode.
 * -------------------------------------------------------------------------- */
static void finish_transfer_and_enter_player(void)
{
    app_mode = APP_MODE_BUSY;

    if (lvgl_port_lock(500))
    {
        ui_set_transfer_status("Stopping services...");
        lvgl_port_unlock();
    }

    http_upload_server_stop(); /* <-- STOP HTTP HERE */
    wifi_transfer_stop();

    /* Give the system time to finish cleanup */
    vTaskDelay(pdMS_TO_TICKS(500));

    if (lvgl_port_lock(500))
    {
        ui_set_transfer_status("Exiting transfer mode...");
        lvgl_port_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(250));

    start_player_mode(true);
}

/* --------------------------------------------------------------------------
 * Application control task
 *
 * This task performs heavy mode transitions:
 * - starting Player mode
 * - starting Transfer mode
 * - finishing Transfer mode
 * -------------------------------------------------------------------------- */
static void app_control_task(void *arg)
{
    (void)arg;

    app_cmd_t cmd;

    while (1)
    {
        if (xQueueReceive(app_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (cmd)
        {
        case APP_CMD_START_PLAYER:
            ESP_LOGI(TAG, "App command: START_PLAYER");
            start_player_mode(false);
            break;

        case APP_CMD_START_TRANSFER:
            ESP_LOGI(TAG, "App command: START_TRANSFER");
            enter_transfer_from_boot_menu();
            break;

        case APP_CMD_FINISH_TRANSFER:
            ESP_LOGI(TAG, "App command: FINISH_TRANSFER");
            finish_transfer_and_enter_player();
            break;

        default:
            ESP_LOGW(TAG, "App command: unknown %d", (int)cmd);
            break;
        }
    }
}

void app_main(void)
{
    /* Initialize display and LVGL UI */
    display_hardware_init();

    /* Create input/application queues */
    input_event_queue = xQueueCreate(32, sizeof(input_event_t));
    player_cmd_queue = xQueueCreate(8, sizeof(player_cmd_t));
    app_cmd_queue = xQueueCreate(8, sizeof(app_cmd_t));

    assert(input_event_queue != NULL);
    assert(player_cmd_queue != NULL);
    assert(app_cmd_queue != NULL);

    /* Mount SD card */
    if (lvgl_port_lock(500))
    {
        ui_set_status("Mounting SD card...");
        lvgl_port_unlock();
    }

    ESP_ERROR_CHECK(mount_sdcard());

    /* Start in boot menu mode */
    app_mode = APP_MODE_BOOT_MENU;

    /* Create application control task */
    xTaskCreate(app_control_task, "app_ctrl", 10240, NULL, 6, NULL);

    /* Initialize encoder/input state machine */
    encoder_input_init();

    /* Show boot menu */
    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(true);
        ui_set_boot_selection(boot_option);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Boot menu active");
}