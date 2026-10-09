/* main/playlist.h */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <stdbool.h>

#include "app_config.h"
#include "cover_cache.h"
#include "esp_err.h"

typedef struct
{
    char files[MAX_FILES][MAX_PATH_LEN];
    cover_entry_t covers[MAX_FILES];
    int count;
    int current_index;
} playlist_t;

esp_err_t playlist_build(playlist_t *playlist);
void playlist_preload_covers(playlist_t *playlist);

#endif