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

/*
 * Call serially from the controller. Rejects a second successful start.
 * The playlist and command queue are borrowed and must remain alive.
 */
esp_err_t audio_player_start(playlist_t *playlist, QueueHandle_t player_cmd_queue);

/* Snapshot only. Change playback through the player command queue. */
bool audio_player_is_playing(void);

/* Publishes the latest volume/mute pair; applied at an output buffer boundary. */
void audio_player_set_volume(uint8_t volume, bool mute);
/* Independent snapshots; two getter calls are not a combined snapshot. */
uint8_t audio_player_get_volume(void);
bool audio_player_is_muted(void);

#endif