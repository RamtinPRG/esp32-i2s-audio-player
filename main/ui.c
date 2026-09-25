#include "ui.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

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
#define UI_EQ_BASELINE_Y (LCD_V_RES - 15)
#define UI_EQ_HEIGHT_MIN 14
#define UI_EQ_HEIGHT_MAX 42
#define UI_EQ_DURATION_MIN 380
#define UI_EQ_DURATION_MAX 620
#define UI_EQ_STAGGER_DELAY 40
#define UI_EQ_COLOR_START 0x00FFFF
#define UI_EQ_COLOR_END 0xE600B3

/* Cover Transition Configuration */
#define COVER_TRANSITION_SLIDE 0
#define COVER_TRANSITION_FADE 1
#define COVER_TRANSITION_ZOOM 2

/* Select the transition effect here */
#define COVER_TRANSITION_MODE COVER_TRANSITION_SLIDE

/* Playback overlay / pause badge */
#define UI_PLAYBACK_OVERLAY_SIZE 56
#define UI_PLAYBACK_SCALE_HIDDEN 192
#define UI_PLAYBACK_SCALE_SHOWN 256

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

    /* New UX Elements */
    lv_obj_t *playback_overlay;
    lv_obj_t *playback_icon;
    bool playback_overlay_visible;
    lv_obj_t *vol_overlay;
    lv_obj_t *vol_bar;
    lv_obj_t *vol_label;
    lv_obj_t *vol_icon;

    bool eq_anim_active;
    uint32_t total_duration_sec;
    uint32_t elapsed_duration_sec;
} ui_ctx_t;

static ui_ctx_t ui_ctx = {0};
static const void *pending_cover_src = NULL;
static bool is_first_cover_shown = false;

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
    return UI_EQ_HEIGHT_MIN + ((i * 67 + 31) % (range + 1));
}

static uint16_t get_dynamic_duration(int i)
{
    int range = UI_EQ_DURATION_MAX - UI_EQ_DURATION_MIN;
    if (range <= 0)
        return UI_EQ_DURATION_MIN;
    return UI_EQ_DURATION_MIN + ((i * 83 + 47) % (range + 1));
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

static void cover_translate_x_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void cover_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void cover_scale_anim_cb(void *var, int32_t v)
{
    lv_image_set_scale((lv_obj_t *)var, v);
}

static void start_cover_in_animation(uint32_t duration)
{
    lv_anim_t a_in;

#if COVER_TRANSITION_MODE == COVER_TRANSITION_SLIDE
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, ui_ctx.cover_img);
    lv_anim_set_values(&a_in, -150, 0);
    lv_anim_set_duration(&a_in, duration);
    lv_anim_set_exec_cb(&a_in, cover_translate_x_cb);
    lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
    lv_anim_start(&a_in);
#elif COVER_TRANSITION_MODE == COVER_TRANSITION_ZOOM
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, ui_ctx.cover_img);
    lv_anim_set_values(&a_in, 0, 256); // 256 is 100% scale in LVGL v9
    lv_anim_set_duration(&a_in, duration);
    lv_anim_set_exec_cb(&a_in, cover_scale_anim_cb);
    lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
    lv_anim_start(&a_in);
#endif

    lv_anim_t a_in_opa;
    lv_anim_init(&a_in_opa);
    lv_anim_set_var(&a_in_opa, ui_ctx.cover_img);
    lv_anim_set_values(&a_in_opa, 0, LV_OPA_COVER);
    lv_anim_set_duration(&a_in_opa, duration);
    lv_anim_set_exec_cb(&a_in_opa, cover_opa_anim_cb);
    lv_anim_start(&a_in_opa);
}

static void cover_transition_out_ready_cb(lv_anim_t *a)
{
    (void)a;
    if (pending_cover_src)
    {
        lv_image_set_src(ui_ctx.cover_img, pending_cover_src);
        lv_image_set_scale(ui_ctx.cover_img, 256);
        pending_cover_src = NULL;

#if COVER_TRANSITION_MODE == COVER_TRANSITION_SLIDE
        lv_obj_set_style_translate_x(ui_ctx.cover_img, -150, LV_PART_MAIN);
        lv_obj_set_style_opa(ui_ctx.cover_img, 0, LV_PART_MAIN);
#elif COVER_TRANSITION_MODE == COVER_TRANSITION_FADE
        lv_obj_set_style_opa(ui_ctx.cover_img, 0, LV_PART_MAIN);
#elif COVER_TRANSITION_MODE == COVER_TRANSITION_ZOOM
        lv_image_set_scale(ui_ctx.cover_img, 0);
        lv_obj_set_style_opa(ui_ctx.cover_img, 0, LV_PART_MAIN);
#endif

        start_cover_in_animation(350);
    }
}

/* --------------------------------------------------------------------------
 * Playback overlay animation helpers
 * -------------------------------------------------------------------------- */

static void playback_overlay_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void playback_overlay_scale_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void playback_overlay_hide_ready_cb(lv_anim_t *a)
{
    (void)a;

    if (ui_ctx.playback_overlay)
    {
        lv_obj_add_flag(ui_ctx.playback_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    ui_ctx.playback_overlay_visible = false;
}

static void show_playback_overlay(void)
{
    if (!ui_ctx.playback_overlay || !ui_ctx.playback_icon)
    {
        return;
    }

    lv_anim_delete(ui_ctx.playback_overlay, playback_overlay_opa_anim_cb);
    lv_anim_delete(ui_ctx.playback_overlay, playback_overlay_scale_anim_cb);

    lv_obj_clear_flag(ui_ctx.playback_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_opa(ui_ctx.playback_overlay, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(ui_ctx.playback_overlay, UI_PLAYBACK_SCALE_HIDDEN, LV_PART_MAIN);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, ui_ctx.playback_overlay);
    lv_anim_set_values(&a_opa, 0, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a_opa, playback_overlay_opa_anim_cb);
    lv_anim_set_duration(&a_opa, 220);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_out);
    lv_anim_start(&a_opa);

    lv_anim_t a_scale;
    lv_anim_init(&a_scale);
    lv_anim_set_var(&a_scale, ui_ctx.playback_overlay);
    lv_anim_set_values(&a_scale, UI_PLAYBACK_SCALE_HIDDEN, UI_PLAYBACK_SCALE_SHOWN);
    lv_anim_set_exec_cb(&a_scale, playback_overlay_scale_anim_cb);
    lv_anim_set_duration(&a_scale, 220);
    lv_anim_set_path_cb(&a_scale, lv_anim_path_ease_out);
    lv_anim_start(&a_scale);

    ui_ctx.playback_overlay_visible = true;
}

static void hide_playback_overlay(void)
{
    if (!ui_ctx.playback_overlay || !ui_ctx.playback_overlay_visible)
    {
        return;
    }

    lv_anim_delete(ui_ctx.playback_overlay, playback_overlay_opa_anim_cb);
    lv_anim_delete(ui_ctx.playback_overlay, playback_overlay_scale_anim_cb);

    int32_t start_opa = lv_obj_get_style_opa(ui_ctx.playback_overlay, LV_PART_MAIN);
    int32_t start_scale = lv_obj_get_style_transform_scale_x(ui_ctx.playback_overlay, LV_PART_MAIN);

    if (start_opa < 0 || start_opa > LV_OPA_COVER)
    {
        start_opa = LV_OPA_COVER;
    }

    if (start_scale <= 0)
    {
        start_scale = UI_PLAYBACK_SCALE_SHOWN;
    }

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, ui_ctx.playback_overlay);
    lv_anim_set_values(&a_opa, start_opa, 0);
    lv_anim_set_exec_cb(&a_opa, playback_overlay_opa_anim_cb);
    lv_anim_set_duration(&a_opa, 180);
    lv_anim_set_path_cb(&a_opa, lv_anim_path_ease_in);
    lv_anim_start(&a_opa);

    lv_anim_t a_scale;
    lv_anim_init(&a_scale);
    lv_anim_set_var(&a_scale, ui_ctx.playback_overlay);
    lv_anim_set_values(&a_scale, start_scale, UI_PLAYBACK_SCALE_HIDDEN);
    lv_anim_set_exec_cb(&a_scale, playback_overlay_scale_anim_cb);
    lv_anim_set_duration(&a_scale, 180);
    lv_anim_set_path_cb(&a_scale, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a_scale, playback_overlay_hide_ready_cb);
    lv_anim_start(&a_scale);

    ui_ctx.playback_overlay_visible = false;
}

void ui_init(void)
{
    ui_ctx.scr = lv_screen_active();
    no_scroll(ui_ctx.scr);
    lv_obj_set_style_bg_color(ui_ctx.scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Artwork Container Card */
    ui_ctx.artwork_card = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.artwork_card);
    lv_obj_set_size(ui_ctx.artwork_card, 140, 140);
    lv_obj_align(ui_ctx.artwork_card, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(ui_ctx.artwork_card, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.artwork_card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.artwork_card, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.artwork_card, 1, LV_PART_MAIN);

    /* Vinyl Placeholder */
    ui_ctx.vinyl_outer = lv_obj_create(ui_ctx.artwork_card);
    no_scroll(ui_ctx.vinyl_outer);
    lv_obj_set_size(ui_ctx.vinyl_outer, 120, 120);
    lv_obj_center(ui_ctx.vinyl_outer);
    lv_obj_set_style_bg_color(ui_ctx.vinyl_outer, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.vinyl_outer, 60, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.vinyl_outer, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.vinyl_outer, 1, LV_PART_MAIN);

    ui_ctx.vinyl_inner = lv_obj_create(ui_ctx.vinyl_outer);
    no_scroll(ui_ctx.vinyl_inner);
    lv_obj_set_size(ui_ctx.vinyl_inner, 30, 30);
    lv_obj_center(ui_ctx.vinyl_inner);
    lv_obj_set_style_bg_color(ui_ctx.vinyl_inner, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.vinyl_inner, 15, LV_PART_MAIN);

    /* Album Cover Image */
    ui_ctx.cover_img = lv_image_create(ui_ctx.scr);
    lv_image_set_inner_align(ui_ctx.cover_img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(ui_ctx.cover_img, 140, 140);
    lv_obj_align(ui_ctx.cover_img, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_flag(ui_ctx.cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(ui_ctx.cover_img, 20, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ui_ctx.cover_img, true, LV_PART_MAIN);

    /* Playback overlay badge centered over artwork / vinyl */
    ui_ctx.playback_overlay = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.playback_overlay);

    lv_obj_set_size(ui_ctx.playback_overlay, UI_PLAYBACK_OVERLAY_SIZE, UI_PLAYBACK_OVERLAY_SIZE);
    lv_obj_align_to(ui_ctx.playback_overlay, ui_ctx.artwork_card, LV_ALIGN_CENTER, 0, 0);

    lv_obj_set_style_bg_color(ui_ctx.playback_overlay, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.playback_overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.playback_overlay, UI_PLAYBACK_OVERLAY_SIZE / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.playback_overlay, 0, LV_PART_MAIN);

    lv_obj_set_style_opa(ui_ctx.playback_overlay, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(ui_ctx.playback_overlay, UI_PLAYBACK_OVERLAY_SIZE / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(ui_ctx.playback_overlay, UI_PLAYBACK_OVERLAY_SIZE / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(ui_ctx.playback_overlay, UI_PLAYBACK_SCALE_SHOWN, LV_PART_MAIN);

    lv_obj_remove_flag(ui_ctx.playback_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_ctx.playback_overlay, LV_OBJ_FLAG_HIDDEN);

    ui_ctx.playback_icon = lv_label_create(ui_ctx.playback_overlay);
    lv_label_set_text(ui_ctx.playback_icon, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(ui_ctx.playback_icon, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ctx.playback_icon, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(ui_ctx.playback_icon);

    /* Track Label */
    ui_ctx.track_label = lv_label_create(ui_ctx.scr);
    lv_obj_set_width(ui_ctx.track_label, 200);
    lv_label_set_long_mode(ui_ctx.track_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(ui_ctx.track_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ctx.track_label, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ctx.track_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(ui_ctx.track_label, 10000, LV_PART_MAIN);
    lv_label_set_text(ui_ctx.track_label, "Waiting for track...");
    lv_obj_align(ui_ctx.track_label, LV_ALIGN_TOP_MID, 0, 175);

    /* Timers & Progress Bar */
    ui_ctx.elapsed_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.elapsed_label, "00:00");
    lv_obj_set_style_text_color(ui_ctx.elapsed_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);
    lv_obj_set_size(ui_ctx.elapsed_label, 40, LV_SIZE_CONTENT);
    lv_obj_align(ui_ctx.elapsed_label, LV_ALIGN_TOP_LEFT, 20, 200);

    ui_ctx.total_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.total_label, "00:00");
    lv_obj_set_style_text_color(ui_ctx.total_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);
    lv_obj_set_size(ui_ctx.total_label, 40, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(ui_ctx.total_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(ui_ctx.total_label, LV_ALIGN_TOP_RIGHT, -20, 200);

    ui_ctx.progress_bar = lv_bar_create(ui_ctx.scr);
    lv_obj_set_size(ui_ctx.progress_bar, 112, 4);
    lv_obj_align(ui_ctx.progress_bar, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_set_style_bg_color(ui_ctx.progress_bar, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ctx.progress_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui_ctx.progress_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.progress_bar, 2, LV_PART_INDICATOR);
    lv_bar_set_value(ui_ctx.progress_bar, 0, LV_ANIM_OFF);

    /* Equalizer Visualizer Bars */
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

    /* Volume Overlay (Hidden by default) */
    ui_ctx.vol_overlay = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.vol_overlay);
    lv_obj_set_size(ui_ctx.vol_overlay, 200, 60);
    lv_obj_align(ui_ctx.vol_overlay, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(ui_ctx.vol_overlay, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.vol_overlay, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.vol_overlay, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.vol_overlay, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.vol_overlay, 1, LV_PART_MAIN);
    lv_obj_add_flag(ui_ctx.vol_overlay, LV_OBJ_FLAG_HIDDEN);

    ui_ctx.vol_icon = lv_label_create(ui_ctx.vol_overlay);
    lv_obj_set_style_text_color(ui_ctx.vol_icon, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ctx.vol_icon, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(ui_ctx.vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(ui_ctx.vol_icon, LV_ALIGN_LEFT_MID, 15, 0);

    ui_ctx.vol_bar = lv_bar_create(ui_ctx.vol_overlay);
    lv_obj_set_size(ui_ctx.vol_bar, 120, 8);
    lv_obj_align(ui_ctx.vol_bar, LV_ALIGN_LEFT_MID, 45, -10);
    lv_obj_set_style_bg_color(ui_ctx.vol_bar, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ctx.vol_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui_ctx.vol_bar, 4, LV_PART_MAIN);
    lv_bar_set_value(ui_ctx.vol_bar, 80, LV_ANIM_OFF);

    ui_ctx.vol_label = lv_label_create(ui_ctx.vol_overlay);
    lv_obj_set_style_text_color(ui_ctx.vol_label, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_ctx.vol_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(ui_ctx.vol_label, "80%");
    lv_obj_align(ui_ctx.vol_label, LV_ALIGN_LEFT_MID, 45, 10);
}

void ui_set_status(const char *text)
{
    if (ui_ctx.track_label && text)
    {
        lv_label_set_text(ui_ctx.track_label, text);
    }
}

void ui_notify_track_started(const char *path, const void *cover_src, int index, int count, uint32_t duration_sec)
{
    (void)index;
    (void)count;
    if (!path || !ui_ctx.track_label)
        return;

    if (cover_src)
    {
        lv_obj_add_flag(ui_ctx.artwork_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_ctx.cover_img, LV_OBJ_FLAG_HIDDEN);

        lv_anim_delete(ui_ctx.cover_img, cover_translate_x_cb);
        lv_anim_delete(ui_ctx.cover_img, cover_opa_anim_cb);
        lv_anim_delete(ui_ctx.cover_img, cover_scale_anim_cb);

        if (!is_first_cover_shown)
        {
            is_first_cover_shown = true;

#if COVER_TRANSITION_MODE == COVER_TRANSITION_SLIDE
            lv_obj_set_style_translate_x(ui_ctx.cover_img, -150, LV_PART_MAIN);
            lv_obj_set_style_opa(ui_ctx.cover_img, 0, LV_PART_MAIN);
#elif COVER_TRANSITION_MODE == COVER_TRANSITION_FADE
            lv_obj_set_style_opa(ui_ctx.cover_img, 0, LV_PART_MAIN);
#elif COVER_TRANSITION_MODE == COVER_TRANSITION_ZOOM
            lv_image_set_scale(ui_ctx.cover_img, 0);
            lv_obj_set_style_opa(ui_ctx.cover_img, 0, LV_PART_MAIN);
#endif

            lv_image_set_src(ui_ctx.cover_img, cover_src);
            lv_image_set_scale(ui_ctx.cover_img, 256);
            start_cover_in_animation(350);
        }
        else
        {
            pending_cover_src = cover_src;
            int32_t current_opa = lv_obj_get_style_opa(ui_ctx.cover_img, LV_PART_MAIN);

#if COVER_TRANSITION_MODE == COVER_TRANSITION_SLIDE
            int32_t current_x = lv_obj_get_style_translate_x(ui_ctx.cover_img, LV_PART_MAIN);
            lv_anim_t a_out_x;
            lv_anim_init(&a_out_x);
            lv_anim_set_var(&a_out_x, ui_ctx.cover_img);
            lv_anim_set_values(&a_out_x, current_x, 150);
            lv_anim_set_duration(&a_out_x, 300);
            lv_anim_set_exec_cb(&a_out_x, cover_translate_x_cb);
            lv_anim_set_path_cb(&a_out_x, lv_anim_path_ease_in);
            lv_anim_set_ready_cb(&a_out_x, cover_transition_out_ready_cb);
            lv_anim_start(&a_out_x);
#elif COVER_TRANSITION_MODE == COVER_TRANSITION_ZOOM
            lv_anim_t a_out_scale;
            lv_anim_init(&a_out_scale);
            lv_anim_set_var(&a_out_scale, ui_ctx.cover_img);
            lv_anim_set_values(&a_out_scale, 256, 0);
            lv_anim_set_duration(&a_out_scale, 300);
            lv_anim_set_exec_cb(&a_out_scale, cover_scale_anim_cb);
            lv_anim_set_path_cb(&a_out_scale, lv_anim_path_ease_in);
            lv_anim_set_ready_cb(&a_out_scale, cover_transition_out_ready_cb);
            lv_anim_start(&a_out_scale);
#elif COVER_TRANSITION_MODE == COVER_TRANSITION_FADE
            lv_anim_t a_out_opa_main;
            lv_anim_init(&a_out_opa_main);
            lv_anim_set_var(&a_out_opa_main, ui_ctx.cover_img);
            lv_anim_set_values(&a_out_opa_main, current_opa, 0);
            lv_anim_set_duration(&a_out_opa_main, 300);
            lv_anim_set_exec_cb(&a_out_opa_main, cover_opa_anim_cb);
            lv_anim_set_path_cb(&a_out_opa_main, lv_anim_path_ease_in);
            lv_anim_set_ready_cb(&a_out_opa_main, cover_transition_out_ready_cb);
            lv_anim_start(&a_out_opa_main);
#endif

#if COVER_TRANSITION_MODE != COVER_TRANSITION_FADE
            lv_anim_t a_out_opa;
            lv_anim_init(&a_out_opa);
            lv_anim_set_var(&a_out_opa, ui_ctx.cover_img);
            lv_anim_set_values(&a_out_opa, current_opa, 0);
            lv_anim_set_duration(&a_out_opa, 300);
            lv_anim_set_exec_cb(&a_out_opa, cover_opa_anim_cb);
            lv_anim_start(&a_out_opa);
#endif
        }
    }
    else
    {
        lv_anim_delete(ui_ctx.cover_img, cover_translate_x_cb);
        lv_anim_delete(ui_ctx.cover_img, cover_opa_anim_cb);
        lv_anim_delete(ui_ctx.cover_img, cover_scale_anim_cb);

        lv_obj_set_style_translate_x(ui_ctx.cover_img, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(ui_ctx.cover_img, LV_OPA_COVER, LV_PART_MAIN);
        lv_image_set_scale(ui_ctx.cover_img, 256);

        lv_obj_clear_flag(ui_ctx.artwork_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_ctx.cover_img, LV_OBJ_FLAG_HIDDEN);
    }

    char name[128];
    get_display_name(path, name, sizeof(name));
    lv_label_set_text(ui_ctx.track_label, name);

    ui_ctx.total_duration_sec = duration_sec;
    ui_ctx.elapsed_duration_sec = 0;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", duration_sec / 60, duration_sec % 60);
    lv_label_set_text(ui_ctx.total_label, buf);
    lv_label_set_text(ui_ctx.elapsed_label, "00:00");
    lv_bar_set_value(ui_ctx.progress_bar, 0, LV_ANIM_OFF);

    if (!ui_ctx.eq_anim_active)
    {
        start_eq_animations();
    }
}

void ui_notify_track_finished(const char *path)
{
    (void)path;
    // Seamless transition
}

/* --------------------------------------------------------------------------
 * New UX Functions for Two-Mode State Machine
 * -------------------------------------------------------------------------- */

void ui_show_volume_mode(bool show)
{
    if (ui_ctx.vol_overlay)
    {
        if (show)
        {
            lv_obj_clear_flag(ui_ctx.vol_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(ui_ctx.vol_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_update_volume(uint8_t volume)
{
    if (ui_ctx.vol_bar)
    {
        lv_bar_set_value(ui_ctx.vol_bar, volume, LV_ANIM_ON);
    }
    if (ui_ctx.vol_label)
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", volume);
        lv_label_set_text(ui_ctx.vol_label, buf);
    }
    if (ui_ctx.vol_icon)
    {
        const char *icon = LV_SYMBOL_VOLUME_MAX;
        if (volume == 0)
            icon = LV_SYMBOL_MUTE;
        else if (volume < 50)
            icon = LV_SYMBOL_VOLUME_MID;
        else
            icon = LV_SYMBOL_VOLUME_MAX;

        lv_label_set_text(ui_ctx.vol_icon, icon);
        lv_obj_set_style_text_color(ui_ctx.vol_icon, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    }
}

void ui_update_mute(bool mute)
{
    if (ui_ctx.vol_icon)
    {
        if (mute)
        {
            lv_label_set_text(ui_ctx.vol_icon, LV_SYMBOL_MUTE);
            lv_obj_set_style_text_color(ui_ctx.vol_icon, lv_color_hex(COLOR_ERROR), LV_PART_MAIN);
        }
        else
        {
            // Restore accent color (icon text will be corrected by ui_update_volume if needed)
            lv_obj_set_style_text_color(ui_ctx.vol_icon, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
        }
    }
    if (ui_ctx.vol_label && mute)
    {
        lv_label_set_text(ui_ctx.vol_label, "MUTED");
    }
}

void ui_update_playback(bool playing)
{
    if (!ui_ctx.playback_overlay)
    {
        return;
    }

    if (playing)
    {
        hide_playback_overlay();

        if (!ui_ctx.eq_anim_active)
        {
            start_eq_animations();
        }
    }
    else
    {
        if (ui_ctx.playback_icon)
        {
            lv_label_set_text(ui_ctx.playback_icon, LV_SYMBOL_PAUSE);
        }

        if (!ui_ctx.playback_overlay_visible)
        {
            show_playback_overlay();
        }

        stop_eq_animations();
    }
}

void ui_update_elapsed(uint32_t elapsed_sec)
{
    if (ui_ctx.total_duration_sec == 0)
        return;

    if (elapsed_sec > ui_ctx.total_duration_sec)
    {
        elapsed_sec = ui_ctx.total_duration_sec;
    }

    ui_ctx.elapsed_duration_sec = elapsed_sec;

    if (ui_ctx.elapsed_label)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02lu:%02lu",
                 ui_ctx.elapsed_duration_sec / 60,
                 ui_ctx.elapsed_duration_sec % 60);
        lv_label_set_text(ui_ctx.elapsed_label, buf);
    }

    if (ui_ctx.progress_bar)
    {
        uint32_t percent = (ui_ctx.elapsed_duration_sec * 100) / ui_ctx.total_duration_sec;
        lv_bar_set_value(ui_ctx.progress_bar, percent, LV_ANIM_ON);
    }
}