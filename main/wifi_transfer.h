/* main/wifi_transfer.h */
#ifndef WIFI_TRANSFER_H
#define WIFI_TRANSFER_H

#include "esp_err.h"

esp_err_t wifi_transfer_start(void);
esp_err_t wifi_transfer_stop(void);

#endif