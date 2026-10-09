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
#include "esp_lvgl_port.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#include "mdns.h"
#include "ff.h" /* For FATFS f_getfree */

#include "app_config.h"
#include "bsp_display.h"
#include "bsp_sdcard.h"
#include "audio_output.h"
#include "playlist.h"
#include "audio_player.h"
#include "app_types.h"
#include "input_knob.h"

static const char *TAG = "I2S_SD";

/* --------------------------------------------------------------------------
 * Global playlist and audio system state
 * -------------------------------------------------------------------------- */
static playlist_t playlist;

/* --------------------------------------------------------------------------
 * Encoder / UI mode state machine
 * -------------------------------------------------------------------------- */

typedef enum
{
    UI_MODE_TRACK = 0,
    UI_MODE_VOLUME,
} ui_mode_t;

static QueueHandle_t player_cmd_queue = NULL;

static QueueHandle_t app_cmd_queue = NULL;
static volatile app_mode_t app_mode = APP_MODE_BOOT_MENU;
static boot_option_t boot_option = BOOT_OPT_PLAYER;

static volatile uint8_t app_volume = 100;
static volatile bool app_mute = false;

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
    "#statusInfo{color:#8A93A6;margin-bottom:15px;font-size:14px;}\n"
    ".progress{width:100%;background:#263143;border-radius:4px;overflow:hidden;margin-top:10px;display:none;}\n"
    ".progress-bar{height:8px;background:#00D5FF;width:0%;transition:width 0.2s;}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>Wi-Fi Upload</h1>\n"
    "<div class='card'>\n"
    "<div id='statusInfo'>Loading SD card info...</div>\n"
    "<p>Select <b>.pcm</b> files (Raw 44.1kHz 16-bit Stereo).</p>\n"
    "<input type='file' id='fileInput' multiple accept='.pcm,.bmp'><br>\n"
    "<button id='uploadBtn' onclick='uploadFiles()'>Upload</button>\n"
    "<div class='progress' id='progressContainer'><div class='progress-bar' id='progressBar'></div></div>\n"
    "<div id='status'></div>\n"
    "</div>\n"
    "<script>\n"
    "async function loadStatus(){\n"
    "  try{\n"
    "    const res=await fetch('/status');\n"
    "    const data=await res.json();\n"
    "    document.getElementById('statusInfo').innerText=\n"
    "      'Free Space: '+(data.free_mb>=0?data.free_mb+' MB':'Unknown')+ ' | Tracks: '+data.files.length;\n"
    "  }catch(e){\n"
    "    document.getElementById('statusInfo').innerText='Error reading SD card status.';\n"
    "  }\n"
    "}\n"
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
    "  loadStatus();\n" /* Refresh free space after upload */
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
    "window.onload = loadStatus;\n"
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

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *buf = malloc(2048);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int offset = 0;
    offset += snprintf(buf + offset, 2048 - offset, "{\"free_mb\":");

    /* Calculate free space on SD card */
    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree(SD_MOUNT_POINT, &fre_clust, &fs) == FR_OK)
    {
        /* fre_clust * csize = free sectors. Usually 512 bytes per sector. */
        uint32_t free_mb = (fre_clust * fs->csize) / 2048;
        offset += snprintf(buf + offset, 2048 - offset, "%lu", (unsigned long)free_mb);
    }
    else
    {
        offset += snprintf(buf + offset, 2048 - offset, "-1");
    }

    offset += snprintf(buf + offset, 2048 - offset, ",\"files\":[");

    /* List existing .pcm files */
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (dir)
    {
        struct dirent *entry;
        bool first = true;
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_type == DT_REG)
            {
                const char *name = entry->d_name;
                int len = strlen(name);
                if (len > 4 && strcasecmp(name + len - 4, ".pcm") == 0)
                {
                    if (!first)
                        offset += snprintf(buf + offset, 2048 - offset, ",");
                    offset += snprintf(buf + offset, 2048 - offset, "\"%s\"", name);
                    first = false;
                    if (offset > 1900)
                        break; /* Prevent buffer overflow */
                }
            }
        }
        closedir(dir);
    }

    offset += snprintf(buf + offset, 2048 - offset, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, offset);
    free(buf);
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

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_register_uri_handler(upload_server, &status_uri);

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

    /* Initialize mDNS so users can use http://esp-audio.local */
    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK)
    {
        mdns_hostname_set("esp-audio");
        mdns_instance_name_set("ESP Audio Player Upload");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS started: http://esp-audio.local");
    }
    else
    {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
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

        /* Clean up mDNS */
        mdns_service_remove_all();
        mdns_free();

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

    esp_err_t ret = playlist_build(&playlist);

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

    playlist_preload_covers(&playlist);

    if (lvgl_port_lock(500))
    {
        ui_set_status("Starting playback...");
        lvgl_port_unlock();
    }

    esp_err_t audio_ret = audio_player_start(&playlist, player_cmd_queue);

    if (audio_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start audio player");

        if (lvgl_port_lock(500))
        {
            ui_set_status("Audio init failed!");
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

    audio_player_set_volume(app_volume, app_mute);

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
    audio_player_set_playback(false);

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
    bsp_display_init();

    /* Create input/application queues */
    player_cmd_queue = xQueueCreate(8, sizeof(player_cmd_t));
    app_cmd_queue = xQueueCreate(8, sizeof(app_cmd_t));

    assert(player_cmd_queue != NULL);
    assert(app_cmd_queue != NULL);

    /* Mount SD card */
    if (lvgl_port_lock(500))
    {
        ui_set_status("Mounting SD card...");
        lvgl_port_unlock();
    }

    ESP_ERROR_CHECK(bsp_sdcard_mount());

    /* Start in boot menu mode */
    app_mode = APP_MODE_BOOT_MENU;

    /* Create application control task */
    xTaskCreate(app_control_task, "app_ctrl", 10240, NULL, 6, NULL);

    /* Initialize encoder/input state machine */
    input_knob_config_t input_cfg = {
        .player_cmd_queue = player_cmd_queue,
        .app_cmd_queue = app_cmd_queue,
        .app_mode = &app_mode,
        .boot_option = &boot_option,
        .volume = &app_volume,
        .mute = &app_mute,
    };

    ESP_ERROR_CHECK(input_knob_init(&input_cfg));

    /* Show boot menu */
    if (lvgl_port_lock(500))
    {
        ui_show_boot_menu(true);
        ui_set_boot_selection(boot_option);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Boot menu active");
}