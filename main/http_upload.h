/* main/http_upload.h */
#ifndef HTTP_UPLOAD_H
#define HTTP_UPLOAD_H

#include "esp_err.h"

esp_err_t http_upload_server_start(void);
esp_err_t http_upload_server_stop(void);

#endif