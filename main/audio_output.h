/* main/audio_output.h */
#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include "driver/i2s_std.h"
#include "esp_err.h"

/*
 * Pass a pointer to a NULL handle. On return, the caller owns any non-NULL
 * handle, including on initialization failure, and must eventually delete it.
 */
esp_err_t audio_output_init(i2s_chan_handle_t *tx_handle);

#endif