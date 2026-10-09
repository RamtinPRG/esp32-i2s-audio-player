/* main/audio_output.h */
#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include "driver/i2s_std.h"
#include "esp_err.h"

esp_err_t audio_output_init(i2s_chan_handle_t *tx_handle);

#endif