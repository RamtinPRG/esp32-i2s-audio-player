#include <stdio.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "lvgl.h"
#include "ui.h"

#define LV_VERSION_MAJOR 9

#define WIDTH 240
#define HEIGHT 280

/* Simulated playlist for testing */
typedef struct
{
    const char *path;
    uint32_t duration;
} mock_track_t;

static const mock_track_t MOCK_PLAYLIST[] = {
    {"/sd/01_Synthwave_Night.pcm", 195},
    {"/sd/02_Cyberpunk_Beat.pcm", 240},
    {"/sd/03_Retro_Groove.pcm", 142},
};
static const int MOCK_COUNT = 3;
static int current_track = 0;

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    lv_init();

#if LV_VERSION_MAJOR >= 9
    /* LVGL 9 Native SDL Display & Input Drivers */
    lv_display_t *disp = lv_sdl_window_create(WIDTH, HEIGHT);
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();
#else
    /* LVGL 8 Driver setup (if using LVGL 8.x SDL port) */
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /* ... LVGL 8 SDL driver init ... */
#endif

    /* Initialize your decoupled UI */
    ui_init();

    /* Set initial UI testing state */
    ui_set_status("Starting playback...");
    ui_notify_track_started(
        MOCK_PLAYLIST[current_track].path,
        current_track + 1,
        MOCK_COUNT,
        MOCK_PLAYLIST[current_track].duration);

    printf("\n=== SDL UI Simulator Controls ===\n");
    printf(" [Space] : Next track\n");
    printf(" [1]     : Simulate 'Loading' state\n");
    printf(" [2]     : Simulate 'Error' state\n");
    printf(" [3]     : Simulate 'Ready' state\n");
    printf(" [ESC]   : Quit\n=================================\n\n");

    bool running = true;
    SDL_Event event;

    while (running)
    {
        /* Process SDL Events (Keyboard controls for UI testing) */
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
                    ui_notify_track_started(
                        MOCK_PLAYLIST[current_track].path,
                        current_track + 1,
                        MOCK_COUNT,
                        MOCK_PLAYLIST[current_track].duration);
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

        /* Run LVGL timers and UI animations */
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms < 1)
            sleep_ms = 1;
        if (sleep_ms > 16)
            sleep_ms = 16; // Cap at ~60 FPS
        SDL_Delay(sleep_ms);
    }

    return 0;
}