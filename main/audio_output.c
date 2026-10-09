/* main/audio_output.c */
#include "app_config.h"
#include "audio_output.h"

#include "esp_log.h"

static const char *TAG = "AUDIO_OUTPUT";

esp_err_t audio_output_init(i2s_chan_handle_t *tx_handle)
{
    if (tx_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCLK,
            .ws = I2S_PIN_WS,
            .dout = I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(*tx_handle, &std_cfg));

    ESP_LOGI(TAG, "I2S output initialized");

    return ESP_OK;
}