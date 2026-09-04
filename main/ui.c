#include "ui.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define LVGL_VERSION_MAJOR 9

#if LVGL_VERSION_MAJOR >= 9
#define GET_ACTIVE_SCREEN() lv_screen_active()
#define LV_LABEL_LONG_SCROLL_CIRC_COMPAT LV_LABEL_LONG_SCROLL_CIRCULAR
#else
#define GET_ACTIVE_SCREEN() lv_scr_act()
#define LV_LABEL_LONG_SCROLL_CIRC_COMPAT LV_LABEL_LONG_SCROLL_CIRCULAR
#endif

#define LCD_H_RES 240
#define LCD_V_RES 280

/* Color Palette */
#define COLOR_BG 0x08090C
#define COLOR_CARD 0x121620
#define COLOR_PRIMARY 0xF2F6FF
#define COLOR_SECONDARY 0x8A93A6
#define COLOR_ACCENT 0x00D5FF
#define COLOR_BORDER 0x263143
#define COLOR_ERROR 0xFF4B7E

/* Visualizer Configuration */
#define UI_EQ_BAR_COUNT 7
#define UI_EQ_BAR_WIDTH 8
#define UI_EQ_BAR_GAP 5
#define UI_EQ_BASE_HEIGHT 6
#define UI_EQ_BASELINE_Y (LCD_V_RES - 15) // Moved lower
#define UI_EQ_HEIGHT_MIN 14
#define UI_EQ_HEIGHT_MAX 42
#define UI_EQ_DURATION_MIN 380
#define UI_EQ_DURATION_MAX 620
#define UI_EQ_STAGGER_DELAY 40
#define UI_EQ_COLOR_START 0x00FFFF
#define UI_EQ_COLOR_END 0xE600B3

typedef struct
{
    lv_obj_t *scr;
    lv_obj_t *artwork_card;
    lv_obj_t *vinyl_outer;
    lv_obj_t *vinyl_inner;
    lv_obj_t *cover_img;
    lv_obj_t *track_label;
    lv_obj_t *elapsed_label;
    lv_obj_t *progress_bar;
    lv_obj_t *total_label;
    lv_obj_t *eq_bars[UI_EQ_BAR_COUNT];
    bool eq_anim_active;
    lv_timer_t *progress_timer;
    uint32_t total_duration_sec;
    uint32_t elapsed_duration_sec;
} ui_ctx_t;

static ui_ctx_t ui_ctx = {0};

static void no_scroll(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}

static void get_display_name(const char *path, char *out, size_t out_len)
{
    if (!path || !out || !out_len)
        return;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, out_len, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot)
    {
        if (strcasecmp(dot, ".pcm") == 0 || strcasecmp(dot, ".mp3") == 0 ||
            strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".flac") == 0)
        {
            *dot = '\0';
        }
    }
}

static void anim_eq_height_cb(void *obj, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)obj, v);
    lv_obj_set_y((lv_obj_t *)obj, UI_EQ_BASELINE_Y - v);
}

static uint8_t get_dynamic_height(int i)
{
    int range = UI_EQ_HEIGHT_MAX - UI_EQ_HEIGHT_MIN;
    if (range <= 0)
        return UI_EQ_HEIGHT_MIN;
    int val = (i * 67 + 31) % (range + 1);
    return UI_EQ_HEIGHT_MIN + val;
}

static uint16_t get_dynamic_duration(int i)
{
    int range = UI_EQ_DURATION_MAX - UI_EQ_DURATION_MIN;
    if (range <= 0)
        return UI_EQ_DURATION_MIN;
    int val = (i * 83 + 47) % (range + 1);
    return UI_EQ_DURATION_MIN + val;
}

static uint32_t interpolate_color(uint32_t c1, uint32_t c2, int i, int total)
{
    if (total <= 1)
        return c1;
    int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    int r = r1 + ((r2 - r1) * i) / (total - 1);
    int g = g1 + ((g2 - g1) * i) / (total - 1);
    int b = b1 + ((b2 - b1) * i) / (total - 1);
    return (r << 16) | (g << 8) | b;
}

static void start_eq_animations(void)
{
    if (ui_ctx.eq_anim_active)
        return;
    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        if (!ui_ctx.eq_bars[i])
            continue;
        uint8_t max_h = get_dynamic_height(i);
        uint16_t dur = get_dynamic_duration(i);
        lv_anim_delete(ui_ctx.eq_bars[i], anim_eq_height_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_ctx.eq_bars[i]);
        lv_anim_set_values(&a, UI_EQ_BASE_HEIGHT, max_h);
        lv_anim_set_exec_cb(&a, anim_eq_height_cb);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_playback_duration(&a, dur);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, i * UI_EQ_STAGGER_DELAY);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
    ui_ctx.eq_anim_active = true;
}

static void stop_eq_animations(void)
{
    if (!ui_ctx.eq_anim_active)
        return;
    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        if (!ui_ctx.eq_bars[i])
            continue;
        lv_anim_delete(ui_ctx.eq_bars[i], anim_eq_height_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_ctx.eq_bars[i]);
        lv_anim_set_values(&a, lv_obj_get_height(ui_ctx.eq_bars[i]), UI_EQ_BASE_HEIGHT);
        lv_anim_set_exec_cb(&a, anim_eq_height_cb);
        lv_anim_set_duration(&a, 280);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_start(&a);
    }
    ui_ctx.eq_anim_active = false;
}

static void progress_timer_cb(lv_timer_t *timer)
{
    if (ui_ctx.total_duration_sec == 0)
        return;
    if (ui_ctx.elapsed_duration_sec < ui_ctx.total_duration_sec)
    {
        ui_ctx.elapsed_duration_sec++;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02lu:%02lu", ui_ctx.elapsed_duration_sec / 60, ui_ctx.elapsed_duration_sec % 60);
        if (ui_ctx.elapsed_label)
            lv_label_set_text(ui_ctx.elapsed_label, buf);
        uint32_t percent = (ui_ctx.elapsed_duration_sec * 100) / ui_ctx.total_duration_sec;
        if (ui_ctx.progress_bar)
            lv_bar_set_value(ui_ctx.progress_bar, percent, LV_ANIM_ON);
    }
}

void ui_init(void)
{
    ui_ctx.scr = GET_ACTIVE_SCREEN();
    no_scroll(ui_ctx.scr);
    lv_obj_set_style_bg_color(ui_ctx.scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.scr, LV_OPA_COVER, LV_PART_MAIN);

    ui_ctx.artwork_card = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.artwork_card);
    lv_obj_set_size(ui_ctx.artwork_card, 140, 140); // Enlarged
    lv_obj_align(ui_ctx.artwork_card, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(ui_ctx.artwork_card, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.artwork_card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.artwork_card, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.artwork_card, 1, LV_PART_MAIN);

    ui_ctx.vinyl_outer = lv_obj_create(ui_ctx.artwork_card);
    no_scroll(ui_ctx.vinyl_outer);
    lv_obj_set_size(ui_ctx.vinyl_outer, 120, 120); // Enlarged to match
    lv_obj_center(ui_ctx.vinyl_outer);
    lv_obj_set_style_bg_color(ui_ctx.vinyl_outer, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.vinyl_outer, 60, LV_PART_MAIN); // Enlarged radius
    lv_obj_set_style_border_color(ui_ctx.vinyl_outer, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.vinyl_outer, 1, LV_PART_MAIN);

    ui_ctx.vinyl_inner = lv_obj_create(ui_ctx.vinyl_outer);
    no_scroll(ui_ctx.vinyl_inner);
    lv_obj_set_size(ui_ctx.vinyl_inner, 30, 30); // Enlarged
    lv_obj_center(ui_ctx.vinyl_inner);
    lv_obj_set_style_bg_color(ui_ctx.vinyl_inner, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.vinyl_inner, 15, LV_PART_MAIN); // Enlarged radius

#if LVGL_VERSION_MAJOR >= 9
    ui_ctx.cover_img = lv_image_create(ui_ctx.scr);
    lv_image_set_inner_align(ui_ctx.cover_img, LV_IMAGE_ALIGN_STRETCH);
#else
    ui_ctx.cover_img = lv_img_create(ui_ctx.scr);
#endif

    lv_obj_set_size(ui_ctx.cover_img, 140, 140); // Enlarged
    lv_obj_align(ui_ctx.cover_img, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_flag(ui_ctx.cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(ui_ctx.cover_img, 20, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ui_ctx.cover_img, true, LV_PART_MAIN);

    // --- Track Label ---
    ui_ctx.track_label = lv_label_create(ui_ctx.scr);
    lv_obj_set_width(ui_ctx.track_label, 200);
    lv_label_set_long_mode(ui_ctx.track_label, LV_LABEL_LONG_SCROLL_CIRC_COMPAT);
    lv_obj_set_style_text_align(ui_ctx.track_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ctx.track_label, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);

    // Set a slightly bigger font (Ensure LV_FONT_MONTSERRAT_16 is set to 1 in your lv_conf.h)
    lv_obj_set_style_text_font(ui_ctx.track_label, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_set_style_anim_time(ui_ctx.track_label, 10000, LV_PART_MAIN);
    lv_label_set_text(ui_ctx.track_label, "Waiting for track...");
    lv_obj_align(ui_ctx.track_label, LV_ALIGN_TOP_MID, 0, 175);

    // --- Elapsed Time Label ---
    ui_ctx.elapsed_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.elapsed_label, "00:00");
    lv_obj_set_style_text_color(ui_ctx.elapsed_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);
    lv_obj_set_size(ui_ctx.elapsed_label, 40, LV_SIZE_CONTENT);
    lv_obj_align(ui_ctx.elapsed_label, LV_ALIGN_TOP_LEFT, 20, 200); // Shifted Y from 215 to 200

    // --- Total Time Label ---
    ui_ctx.total_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.total_label, "00:00");
    lv_obj_set_style_text_color(ui_ctx.total_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);
    lv_obj_set_size(ui_ctx.total_label, 40, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(ui_ctx.total_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(ui_ctx.total_label, LV_ALIGN_TOP_RIGHT, -20, 200); // Shifted Y from 215 to 200

    // --- Progress Bar ---
    ui_ctx.progress_bar = lv_bar_create(ui_ctx.scr);
    lv_obj_set_size(ui_ctx.progress_bar, 112, 4);
    lv_obj_align(ui_ctx.progress_bar, LV_ALIGN_TOP_MID, 0, 204); // Shifted Y from 219 to 204
    lv_obj_set_style_bg_color(ui_ctx.progress_bar, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ctx.progress_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui_ctx.progress_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.progress_bar, 2, LV_PART_INDICATOR);
    lv_bar_set_value(ui_ctx.progress_bar, 0, LV_ANIM_OFF);

    int total_w = UI_EQ_BAR_COUNT * UI_EQ_BAR_WIDTH + (UI_EQ_BAR_COUNT - 1) * UI_EQ_BAR_GAP;
    int start_x = (LCD_H_RES - total_w) / 2;

    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        ui_ctx.eq_bars[i] = lv_obj_create(ui_ctx.scr);
        no_scroll(ui_ctx.eq_bars[i]);
        lv_obj_set_size(ui_ctx.eq_bars[i], UI_EQ_BAR_WIDTH, UI_EQ_BASE_HEIGHT);
        int16_t x_offset = (int16_t)(start_x + i * (UI_EQ_BAR_WIDTH + UI_EQ_BAR_GAP) - (LCD_H_RES / 2));
        lv_obj_align(ui_ctx.eq_bars[i], LV_ALIGN_TOP_MID, x_offset, UI_EQ_BASELINE_Y - UI_EQ_BASE_HEIGHT);
        uint32_t color = interpolate_color(UI_EQ_COLOR_START, UI_EQ_COLOR_END, i, UI_EQ_BAR_COUNT);
        lv_obj_set_style_bg_color(ui_ctx.eq_bars[i], lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ui_ctx.eq_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(ui_ctx.eq_bars[i], UI_EQ_BAR_WIDTH / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(ui_ctx.eq_bars[i], 0, LV_PART_MAIN);
    }

    ui_ctx.progress_timer = lv_timer_create(progress_timer_cb, 1000, NULL);
}

void ui_set_status(const char *text)
{
    if (ui_ctx.track_label && text)
    {
        lv_label_set_text(ui_ctx.track_label, text);
    }
}

void ui_notify_track_started(const char *path,
                             const void *cover_src,
                             int index,
                             int count,
                             uint32_t duration_sec)
{
    (void)index;
    (void)count;

    if (!path || !ui_ctx.track_label)
    {
        return;
    }

    /*
     * If a predecoded cover exists, use it directly from PSRAM.
     * No SD access, no BMP decode, no LVGL filesystem.
     */
    if (cover_src)
    {
        lv_obj_add_flag(ui_ctx.artwork_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ctx.cover_img, LV_OBJ_FLAG_HIDDEN);

#if LVGL_VERSION_MAJOR >= 9
        lv_image_set_src(ui_ctx.cover_img, cover_src);

        /*
         * The preloaded images are already decoded to 140x140.
         * Reset scale to 100% in case another track changed it.
         */
        lv_image_set_scale(ui_ctx.cover_img, 256);
#else
        lv_img_set_src(ui_ctx.cover_img, cover_src);
        lv_img_set_zoom(ui_ctx.cover_img, LV_IMG_ZOOM_NONE);
#endif
    }
    else
    {
        /*
         * No cover art: show vinyl placeholder.
         */
        lv_obj_clear_flag(ui_ctx.artwork_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_ctx.cover_img, LV_OBJ_FLAG_HIDDEN);
    }

    char name[128];
    get_display_name(path, name, sizeof(name));

    lv_label_set_text(ui_ctx.track_label, name);

    ui_ctx.total_duration_sec = duration_sec;
    ui_ctx.elapsed_duration_sec = 0;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(duration_sec / 60),
             (unsigned long)(duration_sec % 60));

    lv_label_set_text(ui_ctx.total_label, buf);
    lv_label_set_text(ui_ctx.elapsed_label, "00:00");
    lv_bar_set_value(ui_ctx.progress_bar, 0, LV_ANIM_OFF);

    if (ui_ctx.progress_timer)
    {
        lv_timer_reset(ui_ctx.progress_timer);
    }

    if (!ui_ctx.eq_anim_active)
    {
        start_eq_animations();
    }
}

void ui_notify_track_finished(const char *path)
{
    // Seamless transition
}