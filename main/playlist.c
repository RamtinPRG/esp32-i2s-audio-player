/* main/playlist.c */
#include "playlist.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PLAYLIST";

static int compare_strings(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

esp_err_t playlist_build(playlist_t *playlist)
{
    DIR *dir = opendir(SD_MOUNT_POINT);

    if (!dir)
    {
        ESP_LOGE(TAG, "Failed to open directory %s", SD_MOUNT_POINT);
        return ESP_FAIL;
    }

    playlist->count = 0;

    struct dirent *entry;

    ESP_LOGI(TAG, "Scanning for .pcm files...");

    while ((entry = readdir(dir)) != NULL && playlist->count < MAX_FILES)
    {
        if (entry->d_type == DT_REG)
        {
            const char *name = entry->d_name;
            int len = strlen(name);

            if (len > 4 && strcasecmp(name + len - 4, ".pcm") == 0)
            {
                snprintf(
                    playlist->files[playlist->count],
                    MAX_PATH_LEN,
                    "%s/%s",
                    SD_MOUNT_POINT,
                    name);

                playlist->count++;
            }
        }
    }

    closedir(dir);

    if (playlist->count == 0)
    {
        ESP_LOGE(TAG, "No .pcm files found on SD card!");
        return ESP_ERR_NOT_FOUND;
    }

    qsort(
        playlist->files,
        playlist->count,
        MAX_PATH_LEN,
        compare_strings);

    ESP_LOGI(TAG, "Found %d PCM file(s):", playlist->count);

    for (int i = 0; i < playlist->count; i++)
    {
        ESP_LOGI(TAG, "  [%d] %s", i + 1, playlist->files[i]);
    }

    playlist->current_index = 0;

    return ESP_OK;
}

void playlist_preload_covers(playlist_t *playlist)
{
    int loaded = 0;

    for (int i = 0; i < playlist->count; i++)
    {
        if (cover_cache_load_one(playlist->files[i], &playlist->covers[i]))
        {
            loaded++;
        }

        /*
         * Optional: yield briefly so long preload loops do not starve
         * lower priority tasks.
         */
        if ((i % 16) == 15)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    ESP_LOGI(
        TAG,
        "Preloaded %d/%d cover images into PSRAM",
        loaded,
        playlist->count);
}