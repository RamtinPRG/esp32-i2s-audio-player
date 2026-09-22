#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ui.h"

#define LVGL_VERSION_MAJOR 9

#define WIDTH 240
#define HEIGHT 280

/* --------------------------------------------------------------------------
 * Cover Art Loader (Simplified for SDL Simulator)
 * Mirrors cover_cache.c but uses standard POSIX memory allocation.
 * -------------------------------------------------------------------------- */
#define COVER_ART_SIZE 140
#define COVER_OUT_BYTES (COVER_ART_SIZE * COVER_ART_SIZE * sizeof(uint16_t))

typedef struct
{
    bool valid;
    lv_image_dsc_t dsc;
    void *pixel_mem;
} cover_entry_t;

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool make_bmp_path(const char *audio_path, char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s", audio_path);
    if (n < 0 || (size_t)n >= out_len)
        return false;
    char *dot = strrchr(out, '.');
    if (!dot)
        return false;
    if (strcasecmp(dot, ".pcm") != 0)
        return false;
    strcpy(dot, ".bmp");
    return true;
}

static bool load_cover_art(const char *audio_path, cover_entry_t *out)
{
    if (!audio_path || !out)
        return false;
    memset(out, 0, sizeof(*out));

    char bmp_path[512];
    if (!make_bmp_path(audio_path, bmp_path, sizeof(bmp_path)))
        return false;

    FILE *f = fopen(bmp_path, "rb");
    if (!f)
        return false;

    bool ok = false;
    uint8_t hdr[54];
    uint8_t *row = NULL;
    uint16_t *pixels = NULL;

    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        goto cleanup;
    if (hdr[0] != 'B' || hdr[1] != 'M')
        goto cleanup;

    uint32_t data_offset = rd_u32(hdr + 10);
    uint32_t dib_size = rd_u32(hdr + 14);
    if (data_offset < sizeof(hdr) || dib_size < 40)
        goto cleanup;

    int32_t src_w = rd_i32(hdr + 18);
    int32_t src_h_signed = rd_i32(hdr + 22);
    uint16_t bpp = rd_u16(hdr + 28);
    uint32_t compression = rd_u32(hdr + 30);

    if (src_w <= 0 || src_h_signed == 0)
        goto cleanup;
    bool top_down = src_h_signed < 0;
    int32_t src_h = top_down ? -src_h_signed : src_h_signed;
    if (src_w > 4096 || src_h > 4096)
        goto cleanup;
    if (compression != 0)
        goto cleanup;
    if (bpp != 24 && bpp != 32)
        goto cleanup;

    int bytes_pp = bpp / 8;
    int row_stride = (src_w * bytes_pp + 3) & ~3;
    row = malloc(row_stride);
    if (!row)
        goto cleanup;

    pixels = malloc(COVER_OUT_BYTES);
    if (!pixels)
        goto cleanup;

    // Decode and nearest-neighbor resize to 140x140
    for (int y = 0; y < COVER_ART_SIZE; y++)
    {
        int sy = (y * src_h) / COVER_ART_SIZE;
        if (sy >= src_h)
            sy = src_h - 1;
        int bmp_row = top_down ? sy : (src_h - 1 - sy);
        long pos = (long)data_offset + (long)bmp_row * row_stride;
        if (fseek(f, pos, SEEK_SET) != 0)
            goto cleanup;
        if (fread(row, 1, row_stride, f) != (size_t)row_stride)
            goto cleanup;

        uint16_t *dst = pixels + (y * COVER_ART_SIZE);
        if (bpp == 24)
        {
            for (int x = 0; x < COVER_ART_SIZE; x++)
            {
                int sx = (x * src_w) / COVER_ART_SIZE;
                if (sx >= src_w)
                    sx = src_w - 1;
                const uint8_t *p = row + sx * 3;
                dst[x] = rgb565(p[2], p[1], p[0]); // BMP is BGR
            }
        }
        else
        {
            for (int x = 0; x < COVER_ART_SIZE; x++)
            {
                int sx = (x * src_w) / COVER_ART_SIZE;
                if (sx >= src_w)
                    sx = src_w - 1;
                const uint8_t *p = row + sx * 4;
                dst[x] = rgb565(p[2], p[1], p[0]);
            }
        }
    }

    memset(&out->dsc, 0, sizeof(out->dsc));
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
    out->dsc.data_size = COVER_OUT_BYTES;
    out->dsc.data = (const uint8_t *)pixels;

    out->pixel_mem = pixels;
    out->valid = true;
    ok = true;

cleanup:
    if (f)
        fclose(f);
    if (row)
        free(row);
    if (!ok && pixels)
        free(pixels);
    return ok;
}

static void free_cover_art(cover_entry_t *entry)
{
    if (!entry)
        return;
    if (entry->pixel_mem)
    {
        free(entry->pixel_mem);
        entry->pixel_mem = NULL;
    }
    entry->valid = false;
}

/* --------------------------------------------------------------------------
 * Playlist & State
 * -------------------------------------------------------------------------- */
typedef struct
{
    const char *path;
    uint32_t duration;
    cover_entry_t cover;
} mock_track_t;

static mock_track_t MOCK_PLAYLIST[] = {
    {"../music_output/City of Stars.pcm", 0, {0}},
    {"../music_output/End Of A Journey.pcm", 0, {0}},
    {"../music_output/Merry Go Round of Life.pcm", 0, {0}},
    {"../music_output/Snowman.pcm", 0, {0}},
    {"../music_output/Style.pcm", 0, {0}},
    {"../music_output/Wedding of Love.pcm", 0, {0}},
};
static const int MOCK_COUNT = sizeof(MOCK_PLAYLIST) / sizeof(MOCK_PLAYLIST[0]);
static int current_track = 0;

static bool is_playing = true;
static uint8_t mock_volume = 80;
static bool mock_muted = false;
static bool vol_overlay_visible = false;

static void init_playlist(void)
{
    for (int i = 0; i < MOCK_COUNT; i++)
    {
        // Calculate real duration from PCM file size (44100Hz * 2ch * 2bytes = 176400 bytes/sec)
        FILE *f = fopen(MOCK_PLAYLIST[i].path, "rb");
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fclose(f);
            MOCK_PLAYLIST[i].duration = (uint32_t)(size / 176400.0f);
        }
        else
        {
            MOCK_PLAYLIST[i].duration = 0;
        }

        // Load cover art into LVGL image descriptor
        load_cover_art(MOCK_PLAYLIST[i].path, &MOCK_PLAYLIST[i].cover);
    }
}

static void cleanup_playlist(void)
{
    for (int i = 0; i < MOCK_COUNT; i++)
    {
        free_cover_art(&MOCK_PLAYLIST[i].cover);
    }
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    lv_init();

#if LVGL_VERSION_MAJOR >= 9
    lv_display_t *disp = lv_sdl_window_create(WIDTH, HEIGHT);
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
#else
    /* LVGL 8 Driver setup omitted for brevity */
#endif

    ui_init();
    init_playlist();

    const void *cover_src = MOCK_PLAYLIST[current_track].cover.valid ? &MOCK_PLAYLIST[current_track].cover.dsc : NULL;

    ui_set_status("Starting playback...");
    ui_notify_track_started(
        MOCK_PLAYLIST[current_track].path,
        cover_src,
        current_track + 1,
        MOCK_COUNT,
        MOCK_PLAYLIST[current_track].duration);
    ui_update_playback(is_playing);
    ui_update_volume(mock_volume);

    printf("\n=== SDL UI Simulator Controls ===\n");
    printf(" [Space] : Next track\n");
    printf(" [P]     : Toggle Play / Pause\n");
    printf(" [Up]    : Volume +10\n");
    printf(" [Down]  : Volume -10\n");
    printf(" [M]     : Toggle Mute\n");
    printf(" [V]     : Toggle Volume overlay\n");
    printf(" [1]     : Simulate 'Loading' state\n");
    printf(" [2]     : Simulate 'Error' state\n");
    printf(" [3]     : Simulate 'Ready' state\n");
    printf(" [ESC]   : Quit\n");
    printf("=================================\n");

    bool running = true;
    SDL_Event event;

    while (running)
    {
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
            {
                running = false;
            }
            else if (event.type == SDL_KEYDOWN)
            {
                switch (event.key.keysym.sym)
                {
                case SDLK_ESCAPE:
                    running = false;
                    break;

                case SDLK_SPACE:
                    current_track = (current_track + 1) % MOCK_COUNT;
                    cover_src = MOCK_PLAYLIST[current_track].cover.valid ? &MOCK_PLAYLIST[current_track].cover.dsc : NULL;
                    ui_notify_track_started(
                        MOCK_PLAYLIST[current_track].path,
                        cover_src,
                        current_track + 1,
                        MOCK_COUNT,
                        MOCK_PLAYLIST[current_track].duration);
                    break;

                case SDLK_p:
                    is_playing = !is_playing;
                    ui_update_playback(is_playing);
                    printf("Playback: %s\n", is_playing ? "Playing" : "Paused");
                    break;

                case SDLK_UP:
                    if (mock_volume <= 90)
                        mock_volume += 10;
                    else
                        mock_volume = 100;
                    ui_update_volume(mock_volume);
                    printf("Volume: %u%%\n", mock_volume);
                    break;

                case SDLK_DOWN:
                    if (mock_volume >= 10)
                        mock_volume -= 10;
                    else
                        mock_volume = 0;
                    ui_update_volume(mock_volume);
                    printf("Volume: %u%%\n", mock_volume);
                    break;

                case SDLK_m:
                    mock_muted = !mock_muted;
                    ui_update_mute(mock_muted);
                    printf("Mute: %s\n", mock_muted ? "ON" : "OFF");
                    break;

                case SDLK_v:
                    vol_overlay_visible = !vol_overlay_visible;
                    ui_show_volume_mode(vol_overlay_visible);
                    printf("Volume overlay: %s\n", vol_overlay_visible ? "Shown" : "Hidden");
                    break;

                case SDLK_1:
                    ui_set_status("Mounting SD card...");
                    break;

                case SDLK_2:
                    ui_set_status("Failed to read filesystem!");
                    break;

                case SDLK_3:
                    ui_set_status("Ready");
                    break;

                default:
                    break;
                }
            }
        }

        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms < 1)
            sleep_ms = 1;
        if (sleep_ms > 16)
            sleep_ms = 16;
        SDL_Delay(sleep_ms);
    }

    cleanup_playlist();
    return 0;
}