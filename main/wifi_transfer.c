/* main/wifi_transfer.c */
#include "wifi_transfer.h"

#include "app_config.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_TRANSFER";

static bool s_tcpip_initialized = false;
static bool s_driver_initialized = false;
static bool s_ap_running = false;
static bool s_mdns_started = false;

static esp_netif_t *s_wifi_ap_netif = NULL;
static esp_event_handler_instance_t s_wifi_event_instance = NULL;

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
esp_err_t wifi_transfer_start(void)
{
    esp_err_t ret;

    if (!s_tcpip_initialized)
    {
        ESP_LOGI(TAG, "Initializing NVS / netif / event loop for Wi-Fi");

        /* Initialize NVS */
        ret = nvs_flash_init();

        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        /* Initialize TCP/IP */
        ret = esp_netif_init();

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        /* Create default event loop if it does not already exist */
        ret = esp_event_loop_create_default();

        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
            return ret;
        }

        s_tcpip_initialized = true;
    }

    if (!s_driver_initialized)
    {
        ESP_LOGI(TAG, "Creating Wi-Fi AP interface");

        /* Create default Wi-Fi AP network interface */
        s_wifi_ap_netif = esp_netif_create_default_wifi_ap();

        if (s_wifi_ap_netif == NULL)
        {
            ESP_LOGE(TAG, "Failed to create Wi-Fi AP netif");
            return ESP_FAIL;
        }

        /* Configure static IP: 192.168.4.1 */
        esp_netif_ip_info_t ip_info;

        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

        /* Ignore result here: DHCP server may already be stopped */
        esp_netif_dhcps_stop(s_wifi_ap_netif);

        ret = esp_netif_set_ip_info(s_wifi_ap_netif, &ip_info);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to set AP IP info: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_netif_dhcps_start(s_wifi_ap_netif);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to start DHCP server: %s", esp_err_to_name(ret));
            return ret;
        }

        /* Initialize Wi-Fi */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        ret = esp_wifi_init(&cfg);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        /* Register Wi-Fi event handler */
        ret = esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            &s_wifi_event_instance);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(ret));
            return ret;
        }

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

        ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_wifi_set_mode(WIFI_MODE_AP);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
            return ret;
        }

        s_driver_initialized = true;
    }
    else
    {
        /*
         * If the driver already exists but was only stopped, make sure the
         * DHCP server is running again.
         */
        if (s_wifi_ap_netif != NULL)
        {
            esp_netif_dhcps_start(s_wifi_ap_netif);
        }
    }

    if (!s_ap_running)
    {
        ret = esp_wifi_start();

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
            return ret;
        }

        esp_wifi_set_ps(WIFI_PS_NONE);

        s_ap_running = true;

        ESP_LOGI(TAG,
                 "Wi-Fi transfer AP active: SSID=%s, URL=%s",
                 WIFI_AP_SSID,
                 WIFI_TRANSFER_URL);
    }

    /* Initialize mDNS so users can use http://esp-audio.local */
    if (!s_mdns_started)
    {
        ret = mdns_init();

        if (ret == ESP_OK)
        {
            mdns_hostname_set("esp-audio");
            mdns_instance_name_set("ESP Audio Player Upload");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

            s_mdns_started = true;

            ESP_LOGI(TAG, "mDNS started: http://esp-audio.local");
        }
        else
        {
            ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Stop and fully deinitialize Wi-Fi before entering Player mode.
 *
 * This frees Wi-Fi resources so playback, LVGL, and cover loading have more
 * memory available.
 * -------------------------------------------------------------------------- */
esp_err_t wifi_transfer_stop(void)
{
    if (!s_driver_initialized)
    {
        return ESP_OK;
    }

    if (s_mdns_started)
    {
        mdns_service_remove_all();
        mdns_free();

        s_mdns_started = false;
    }

    if (s_ap_running)
    {
        ESP_LOGI(TAG, "Stopping Wi-Fi transfer AP");

        if (s_wifi_ap_netif != NULL)
        {
            esp_netif_dhcps_stop(s_wifi_ap_netif);
        }

        esp_err_t ret = esp_wifi_stop();

        if (ret == ESP_OK)
        {
            s_ap_running = false;
        }
        else
        {
            ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
        }

        /* Give Wi-Fi a little time to finish disconnect/cleanup */
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    /* Unregister event handler */
    if (s_wifi_event_instance != NULL)
    {
        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_event_instance);

        s_wifi_event_instance = NULL;
    }

    /* Fully deinitialize Wi-Fi driver */
    esp_err_t ret = esp_wifi_deinit();

    if (ret == ESP_OK)
    {
        s_driver_initialized = false;

        if (s_wifi_ap_netif != NULL)
        {
            esp_netif_destroy_default_wifi(s_wifi_ap_netif);
            s_wifi_ap_netif = NULL;
        }

        ESP_LOGI(TAG, "Wi-Fi fully deinitialized");
    }
    else
    {
        ESP_LOGW(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(ret));
    }

    return ret;
}