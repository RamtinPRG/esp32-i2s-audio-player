/* main/audio_player.h */
#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "playlist.h"

typedef enum
{
    PLAYER_CMD_NEXT = 0,
    PLAYER_CMD_PREV,
    PLAYER_CMD_TOGGLE_PLAY,
} player_cmd_t;

esp_err_t audio_player_start(playlist_t *playlist, QueueHandle_t player_cmd_queue);

void audio_player_set_playback(bool playing);
bool audio_player_is_playing(void);

void audio_player_set_volume(uint8_t volume, bool mute);
uint8_t audio_player_get_volume(void);
bool audio_player_is_muted(void);

#endif