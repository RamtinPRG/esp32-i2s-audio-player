#include "cover_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "cover_cache";

#define COVER_MAX_PATH 512
#define COVER_OUT_BYTES (COVER_ART_SIZE * COVER_ART_SIZE * sizeof(uint16_t))

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) |
                      ((g & 0xFC) << 3) |
                      (b >> 3));
}

static void *alloc_psram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
    {
        /* Fallback to internal RAM if PSRAM is unavailable or full. */
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

/*
 * Converts "/sdcard/song.pcm" to "/sdcard/song.bmp".
 */
static bool make_bmp_path(const char *audio_path, char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s", audio_path);
    if (n < 0 || (size_t)n >= out_len)
    {
        return false;
    }

    char *dot = strrchr(out, '.');
    if (!dot)
    {
        return false;
    }

    if (strcasecmp(dot, ".pcm") != 0)
    {
        return false;
    }

    strcpy(dot, ".bmp");
    return true;
}

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t make_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = rgb565(r, g, b);

    // c = swap16(c);

    return c;
}

void cover_cache_free(cover_entry_t *entry)
{
    if (!entry)
    {
        return;
    }

    if (entry->pixel_mem)
    {
        heap_caps_free(entry->pixel_mem);
        entry->pixel_mem = NULL;
    }

    entry->valid = false;
    memset(&entry->dsc, 0, sizeof(entry->dsc));
}

bool cover_cache_load_one(const char *audio_path, cover_entry_t *out)
{
    if (!audio_path || !out)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));

    char bmp_path[COVER_MAX_PATH];
    if (!make_bmp_path(audio_path, bmp_path, sizeof(bmp_path)))
    {
        return false;
    }

    FILE *f = fopen(bmp_path, "rb");
    if (!f)
    {
        ESP_LOGD(TAG, "No cover found: %s", bmp_path);
        return false;
    }

    bool ok = false;
    uint8_t hdr[54];
    uint8_t *row = NULL;
    uint16_t *pixels = NULL;

    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        goto cleanup;
    }

    if (hdr[0] != 'B' || hdr[1] != 'M')
    {
        ESP_LOGW(TAG, "Not a BMP file: %s", bmp_path);
        goto cleanup;
    }

    uint32_t data_offset = rd_u32(hdr + 10);
    uint32_t dib_size = rd_u32(hdr + 14);

    if (data_offset < sizeof(hdr))
    {
        ESP_LOGW(TAG, "Invalid BMP data offset: %s", bmp_path);
        goto cleanup;
    }

    if (dib_size < 40)
    {
        ESP_LOGW(TAG, "Only BITMAPINFOHEADER BMPs are supported: %s", bmp_path);
        goto cleanup;
    }

    int32_t src_w = rd_i32(hdr + 18);
    int32_t src_h_signed = rd_i32(hdr + 22);
    uint16_t bpp = rd_u16(hdr + 28);
    uint32_t compression = rd_u32(hdr + 30);

    if (src_w <= 0 || src_h_signed == 0)
    {
        ESP_LOGW(TAG, "Invalid BMP dimensions: %s", bmp_path);
        goto cleanup;
    }

    bool top_down = src_h_signed < 0;
    int32_t src_h = top_down ? -src_h_signed : src_h_signed;

    if (src_w > 4096 || src_h > 4096)
    {
        ESP_LOGW(TAG, "BMP too large: %s", bmp_path);
        goto cleanup;
    }

    if (compression != 0)
    {
        ESP_LOGW(TAG, "Compressed BMP not supported: %s", bmp_path);
        goto cleanup;
    }

    if (bpp != 24 && bpp != 32)
    {
        ESP_LOGW(TAG, "Unsupported BMP bpp=%u: %s", (unsigned)bpp, bmp_path);
        goto cleanup;
    }

    int bytes_pp = bpp / 8;
    int row_stride = (src_w * bytes_pp + 3) & ~3;

    row = malloc(row_stride);
    if (!row)
    {
        ESP_LOGE(TAG, "Failed to allocate BMP row buffer");
        goto cleanup;
    }

    pixels = alloc_psram(COVER_OUT_BYTES);
    if (!pixels)
    {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for cover %s",
                 (unsigned)COVER_OUT_BYTES, bmp_path);
        goto cleanup;
    }

    /*
     * Decode and resize with nearest-neighbor sampling to 140x140.
     * If your BMPs are already 140x140, this becomes a direct copy.
     */
    for (int y = 0; y < COVER_ART_SIZE; y++)
    {
        int sy = (y * src_h) / COVER_ART_SIZE;
        if (sy >= src_h)
        {
            sy = src_h - 1;
        }

        int bmp_row = top_down ? sy : (src_h - 1 - sy);
        long pos = (long)data_offset + (long)bmp_row * row_stride;

        if (fseek(f, pos, SEEK_SET) != 0)
        {
            goto cleanup;
        }

        if (fread(row, 1, row_stride, f) != (size_t)row_stride)
        {
            goto cleanup;
        }

        uint16_t *dst = pixels + (y * COVER_ART_SIZE);

        if (bpp == 24)
        {
            for (int x = 0; x < COVER_ART_SIZE; x++)
            {
                int sx = (x * src_w) / COVER_ART_SIZE;
                if (sx >= src_w)
                {
                    sx = src_w - 1;
                }

                const uint8_t *p = row + sx * 3;

                uint8_t b = p[0];
                uint8_t g = p[1];
                uint8_t r = p[2];

                dst[x] = make_pixel(r, g, b);
            }
        }
        else /* 32-bit BI_RGB */
        {
            for (int x = 0; x < COVER_ART_SIZE; x++)
            {
                int sx = (x * src_w) / COVER_ART_SIZE;
                if (sx >= src_w)
                {
                    sx = src_w - 1;
                }

                const uint8_t *p = row + sx * 4;

                uint8_t b = p[0];
                uint8_t g = p[1];
                uint8_t r = p[2];

                /* Alpha ignored. Cover art does not need transparency. */
                dst[x] = rgb565(r, g, b);
            }
        }
    }

    memset(&out->dsc, 0, sizeof(out->dsc));

#if LVGL_VERSION_MAJOR >= 9
    /*
     * LVGL 9 variable image sources require a valid image header magic.
     */
#ifdef LV_IMAGE_HEADER_MAGIC
    out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
#else
    out->dsc.header.magic = 0x19;
#endif

    out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    out->dsc.header.flags = 0;
    out->dsc.header.w = COVER_ART_SIZE;
    out->dsc.header.h = COVER_ART_SIZE;
    out->dsc.header.stride = COVER_ART_SIZE * sizeof(uint16_t);

    /*
     * CRITICAL FOR LVGL 9:
     * You must specify the exact size of the pixel data buffer in bytes.
     */
    out->dsc.data_size = COVER_OUT_BYTES;
    out->dsc.data = (const uint8_t *)pixels;
#else
    /*
     * LVGL 8 compatibility.
     * This assumes LV_COLOR_DEPTH is 16.
     */
    out->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    out->dsc.header.w = COVER_ART_SIZE;
    out->dsc.header.h = COVER_ART_SIZE;
    out->dsc.data = (const uint8_t *)pixels;
#endif

    out->pixel_mem = pixels;
    out->valid = true;
    ok = true;

cleanup:
    if (f)
    {
        fclose(f);
    }

    if (row)
    {
        free(row);
    }

    if (!ok && pixels)
    {
        heap_caps_free(pixels);
    }

    return ok;
}