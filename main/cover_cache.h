#ifndef COVER_CACHE_H
#define COVER_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/*
 * LVGL version compatibility.
 * Your ui.c currently assumes LVGL 9.
 */
// #if !defined(LV_VERSION_MAJOR) && defined(LVGL_VERSION_MAJOR)
// #define LVGL_VERSION_MAJOR LVGL_VERSION_MAJOR
// #endif

// #ifndef LVGL_VERSION_MAJOR
// #define LV_VERSION_MAJOR 9
// #endif

/*
 * Your UI cover widget is 140x140.
 * Predecode covers to this exact size so LVGL does not need scaling.
 */
#define COVER_ART_SIZE 140

#if LVGL_VERSION_MAJOR >= 9
typedef lv_image_dsc_t cover_image_dsc_t;
#else
typedef lv_img_dsc_t cover_image_dsc_t;
#endif

typedef struct
{
    bool valid;
    cover_image_dsc_t dsc;
    void *pixel_mem;
} cover_entry_t;

/*
 * Loads "audio_path" with extension replaced by ".bmp".
 * Decodes the BMP into PSRAM as RGB565 and fills out->dsc.
 *
 * Currently supports:
 * - BMP
 * - BITMAPINFOHEADER or larger
 * - BI_RGB uncompressed
 * - 24-bit or 32-bit pixels
 *
 * Returns true on success.
 */
bool cover_cache_load_one(const char *audio_path, cover_entry_t *out);

/*
 * Frees PSRAM owned by a cover entry.
 */
void cover_cache_free(cover_entry_t *entry);

#endif /* COVER_CACHE_H */