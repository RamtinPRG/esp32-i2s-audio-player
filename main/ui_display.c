#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_common.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "ui_display.h"

static const char *TAG = "UI_DISPLAY";

/*
 * ST7789 240x280 wiring.
 * Adjust these for your board.
 */
#define LCD_SPI_HOST SPI3_HOST

#define LCD_PIN_SCLK 10
#define LCD_PIN_MOSI 11
#define LCD_PIN_RST 12
#define LCD_PIN_DC 13
#define LCD_PIN_CS 14

#define LCD_H_RES 240
#define LCD_V_RES 280
#define LCD_BIT_PER_PIXEL 16

/*
 * 40 MHz is usually fine for ST7789.
 */
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

/*
 * LVGL v8/v9 compatibility.
 */
#if LV_VERSION_MAJOR >= 9
#define GET_ACTIVE_SCREEN() lv_screen_active()
#define LV_LABEL_LONG_SCROLL_CIRC_COMPAT LV_LABEL_LONG_SCROLL_CIRCULAR
#else
#define GET_ACTIVE_SCREEN() lv_scr_act()
#define LV_LABEL_LONG_SCROLL_CIRC_COMPAT LV_LABEL_LONG_SCROLL_CIRCULAR
#endif

/*
 * UI layout constants.
 */
#define UI_CARD_TOP 44
#define UI_CARD_WIDTH 216
#define UI_CARD_HEIGHT 92

#define UI_STATUS_TOP (UI_CARD_TOP + UI_CARD_HEIGHT + 10)
#define UI_STATUS_WIDTH 190
#define UI_STATUS_HEIGHT 24

#define UI_EQ_BAR_COUNT 7
#define UI_EQ_BAR_WIDTH 14
#define UI_EQ_BAR_GAP 10
#define UI_EQ_BASE_HEIGHT 10
#define UI_EQ_BASELINE_Y (LCD_V_RES - 32)

typedef struct
{
    lv_obj_t *scr;

    lv_obj_t *header_label;
    lv_obj_t *card;
    lv_obj_t *now_playing_label;
    lv_obj_t *track_label;
    lv_obj_t *track_count_label;

    lv_obj_t *status_pill;
    lv_obj_t *status_label;

    lv_obj_t *eq_line;
    lv_obj_t *eq_bars[UI_EQ_BAR_COUNT];

    bool eq_anim_active;
} ui_ctx_t;

static ui_ctx_t ui_ctx = {0};

/*
 * Animation callbacks.
 */
static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void anim_translate_y_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, LV_PART_MAIN);
}

static void anim_eq_height_cb(void *obj, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)obj, v);
    /*
     * Compensate Y position so EQ bars grow upward.
     */
    lv_obj_set_y((lv_obj_t *)obj, UI_EQ_BASELINE_Y - v);
}

/*
 * Disable scrolling and scrollbars for an object.
 */
static void no_scroll(lv_obj_t *obj)
{
    if (obj == NULL)
        return;

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_pad_top(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN);
}

/*
 * Extract filename and remove common audio extensions.
 */
static void get_display_name(const char *path, char *out, size_t out_len)
{
    if (path == NULL || out == NULL || out_len == 0)
        return;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    snprintf(out, out_len, "%s", base);

    char *dot = strrchr(out, '.');
    if (dot != NULL)
    {
        if (strcasecmp(dot, ".pcm") == 0 ||
            strcasecmp(dot, ".mp3") == 0 ||
            strcasecmp(dot, ".wav") == 0 ||
            strcasecmp(dot, ".flac") == 0)
        {
            *dot = '\0';
        }
    }
}

/*
 * Simple fade-in + upward slide animation.
 */
static void fade_in_obj(lv_obj_t *obj, int32_t y_start, uint32_t delay)
{
    if (obj == NULL)
        return;

    lv_anim_delete(obj, anim_opa_cb);
    lv_anim_delete(obj, anim_translate_y_cb);

    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);

    if (y_start != 0)
    {
        lv_obj_set_style_translate_y(obj, y_start, LV_PART_MAIN);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_duration(&a, 550);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);

    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_start(&a);

    if (y_start != 0)
    {
        lv_anim_set_exec_cb(&a, anim_translate_y_cb);
        lv_anim_set_values(&a, y_start, 0);
        lv_anim_start(&a);
    }
}

/*
 * Start EQ animations.
 */
static void start_eq_animations(void)
{
    if (ui_ctx.eq_anim_active)
        return;

    static const uint8_t max_heights[UI_EQ_BAR_COUNT] = {44, 58, 36, 68, 50, 60, 42};
    static const uint16_t durations[UI_EQ_BAR_COUNT] = {430, 520, 360, 590, 470, 540, 410};

    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        if (ui_ctx.eq_bars[i] == NULL)
            continue;

        lv_anim_delete(ui_ctx.eq_bars[i], anim_eq_height_cb);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_ctx.eq_bars[i]);
        lv_anim_set_values(&a, UI_EQ_BASE_HEIGHT, max_heights[i]);
        lv_anim_set_exec_cb(&a, anim_eq_height_cb);
        lv_anim_set_duration(&a, durations[i]);
        lv_anim_set_playback_duration(&a, durations[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, i * 70);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    ui_ctx.eq_anim_active = true;
}

/*
 * Stop EQ animations and smoothly return bars to base height.
 */
static void stop_eq_animations(void)
{
    if (!ui_ctx.eq_anim_active)
        return;

    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        if (ui_ctx.eq_bars[i] == NULL)
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

/*
 * Helper to update status pill colors.
 */
static void set_status_visual_locked(uint32_t text_color_hex, uint32_t border_color_hex)
{
    if (ui_ctx.status_label != NULL)
    {
        lv_obj_set_style_text_color(ui_ctx.status_label, lv_color_hex(text_color_hex), LV_PART_MAIN);
    }

    if (ui_ctx.status_pill != NULL)
    {
        lv_obj_set_style_border_color(ui_ctx.status_pill, lv_color_hex(border_color_hex), LV_PART_MAIN);
    }
}

/*
 * Build UI.
 */
static void ui_create(void)
{
    ui_ctx.scr = GET_ACTIVE_SCREEN();

    /*
     * FIX: Screen background is now a SOLID flat color.
     * Dark gradients on RGB565 displays cause severe color banding (visible stripes).
     */
    no_scroll(ui_ctx.scr);
    lv_obj_set_style_bg_color(ui_ctx.scr, lv_color_hex(0x08090C), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui_ctx.scr, LV_GRAD_DIR_NONE, LV_PART_MAIN); // Explicitly no gradient
    lv_obj_set_style_bg_opa(ui_ctx.scr, LV_OPA_COVER, LV_PART_MAIN);

    /*
     * Header.
     */
    ui_ctx.header_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.header_label, "ESP32 AUDIO");
    lv_obj_set_style_text_color(ui_ctx.header_label, lv_color_hex(0x7C8494), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(ui_ctx.header_label, 3, LV_PART_MAIN);
    lv_obj_align(ui_ctx.header_label, LV_ALIGN_TOP_MID, 0, 18);

    /*
     * Header underline.
     */
    lv_obj_t *header_line = lv_obj_create(ui_ctx.scr);
    no_scroll(header_line);
    lv_obj_set_size(header_line, 132, 1);
    lv_obj_align(header_line, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(header_line, lv_color_hex(0x1D2738), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header_line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(header_line, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_line, 0, LV_PART_MAIN);

    /*
     * Main card (Solid color to prevent banding).
     */
    ui_ctx.card = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.card);

    lv_obj_set_size(ui_ctx.card, UI_CARD_WIDTH, UI_CARD_HEIGHT);
    lv_obj_align(ui_ctx.card, LV_ALIGN_TOP_MID, 0, UI_CARD_TOP);

    lv_obj_set_style_bg_color(ui_ctx.card, lv_color_hex(0x121620), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui_ctx.card, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.card, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_radius(ui_ctx.card, 16, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.card, lv_color_hex(0x263143), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.card, 1, LV_PART_MAIN);

    /*
     * "NOW PLAYING" label inside card.
     */
    ui_ctx.now_playing_label = lv_label_create(ui_ctx.card);
    lv_label_set_text(ui_ctx.now_playing_label, "NOW PLAYING");
    lv_obj_set_style_text_color(ui_ctx.now_playing_label, lv_color_hex(0x8A93A6), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(ui_ctx.now_playing_label, 2, LV_PART_MAIN);
    lv_obj_align(ui_ctx.now_playing_label, LV_ALIGN_TOP_MID, 0, 10);

    /*
     * Track title.
     */
    ui_ctx.track_label = lv_label_create(ui_ctx.card);
    lv_obj_set_width(ui_ctx.track_label, 192);
    lv_label_set_long_mode(ui_ctx.track_label, LV_LABEL_LONG_SCROLL_CIRC_COMPAT);

    lv_obj_set_style_text_align(ui_ctx.track_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ctx.track_label, lv_color_hex(0xF2F6FF), LV_PART_MAIN);
    lv_obj_set_style_anim_time(ui_ctx.track_label, 12000, LV_PART_MAIN);

    lv_label_set_text(ui_ctx.track_label, "Waiting for first track");
    lv_obj_align(ui_ctx.track_label, LV_ALIGN_CENTER, 0, 4);

    /*
     * Track count.
     */
    ui_ctx.track_count_label = lv_label_create(ui_ctx.card);
    lv_label_set_text(ui_ctx.track_count_label, "-- / --");
    lv_obj_set_style_text_color(ui_ctx.track_count_label, lv_color_hex(0x00CFFF), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(ui_ctx.track_count_label, 1, LV_PART_MAIN);
    lv_obj_align(ui_ctx.track_count_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    /*
     * Status pill (Solid color).
     */
    ui_ctx.status_pill = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.status_pill);

    lv_obj_set_size(ui_ctx.status_pill, UI_STATUS_WIDTH, UI_STATUS_HEIGHT);
    lv_obj_align(ui_ctx.status_pill, LV_ALIGN_TOP_MID, 0, UI_STATUS_TOP);

    lv_obj_set_style_bg_color(ui_ctx.status_pill, lv_color_hex(0x121620), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui_ctx.status_pill, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.status_pill, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_radius(ui_ctx.status_pill, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.status_pill, lv_color_hex(0x22304A), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.status_pill, 1, LV_PART_MAIN);

    /*
     * Status label inside pill.
     */
    ui_ctx.status_label = lv_label_create(ui_ctx.status_pill);
    lv_obj_set_width(ui_ctx.status_label, 170);
    lv_label_set_long_mode(ui_ctx.status_label, LV_LABEL_LONG_DOT);

    lv_obj_set_style_text_align(ui_ctx.status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ctx.status_label, lv_color_hex(0x9FE8FF), LV_PART_MAIN);

    lv_label_set_text(ui_ctx.status_label, "Ready");
    lv_obj_center(ui_ctx.status_label);

    /*
     * EQ baseline (Solid color).
     */
    ui_ctx.eq_line = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.eq_line);

    lv_obj_set_size(ui_ctx.eq_line, 164, 2);
    lv_obj_align(ui_ctx.eq_line, LV_ALIGN_TOP_MID, 0, UI_EQ_BASELINE_Y + 3);

    lv_obj_set_style_bg_color(ui_ctx.eq_line, lv_color_hex(0x2A3245), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui_ctx.eq_line, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.eq_line, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_radius(ui_ctx.eq_line, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.eq_line, 0, LV_PART_MAIN);

    /*
     * EQ bars.
     * Gradients are kept here because the bright colors have enough range
     * to render smoothly without banding on RGB565.
     */
    static const uint32_t eq_base_colors[UI_EQ_BAR_COUNT] =
        {
            0x003B4D, 0x00305F, 0x221A66, 0x3D1064, 0x590C57, 0x60103D, 0x5C1426};

    static const uint32_t eq_top_colors[UI_EQ_BAR_COUNT] =
        {
            0x00E5FF, 0x00A6FF, 0x5E6BFF, 0xA24BFF, 0xFF4BD1, 0xFF4B7E, 0xFF6A5E};

    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        ui_ctx.eq_bars[i] = lv_obj_create(ui_ctx.scr);
        no_scroll(ui_ctx.eq_bars[i]);

        lv_obj_set_size(ui_ctx.eq_bars[i], UI_EQ_BAR_WIDTH, UI_EQ_BASE_HEIGHT);

        int16_t x_offset = (int16_t)((i - (UI_EQ_BAR_COUNT / 2)) * (UI_EQ_BAR_WIDTH + UI_EQ_BAR_GAP));

        lv_obj_align(ui_ctx.eq_bars[i], LV_ALIGN_TOP_MID, x_offset, UI_EQ_BASELINE_Y - UI_EQ_BASE_HEIGHT);

        lv_obj_set_style_bg_grad_dir(ui_ctx.eq_bars[i], LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui_ctx.eq_bars[i], lv_color_hex(eq_base_colors[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(ui_ctx.eq_bars[i], lv_color_hex(eq_top_colors[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ui_ctx.eq_bars[i], LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_set_style_radius(ui_ctx.eq_bars[i], UI_EQ_BAR_WIDTH / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(ui_ctx.eq_bars[i], 0, LV_PART_MAIN);
    }

    /*
     * Entry animations.
     */
    fade_in_obj(ui_ctx.header_label, -8, 0);
    fade_in_obj(header_line, -4, 70);

    fade_in_obj(ui_ctx.card, 16, 140);
    fade_in_obj(ui_ctx.now_playing_label, 8, 220);
    fade_in_obj(ui_ctx.track_label, 10, 260);
    fade_in_obj(ui_ctx.track_count_label, 6, 300);

    fade_in_obj(ui_ctx.status_pill, 10, 360);
    fade_in_obj(ui_ctx.status_label, 6, 400);

    fade_in_obj(ui_ctx.eq_line, 14, 440);

    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        fade_in_obj(ui_ctx.eq_bars[i], 18, 480 + (i * 35));
    }

    set_status_visual_locked(0x9FE8FF, 0x22304A);
}

/*
 * Public API.
 */

void ui_set_status(const char *text)
{
    if (ui_ctx.status_label == NULL || text == NULL)
        return;

    if (lvgl_port_lock(500))
    {
        lv_label_set_text(ui_ctx.status_label, text);
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGW(TAG, "LVGL lock timeout, skipped status update");
    }
}

void ui_notify_track_started(const char *path, int index, int count)
{
    if (path == NULL || ui_ctx.track_label == NULL || ui_ctx.track_count_label == NULL)
        return;

    char name[128];
    get_display_name(path, name, sizeof(name));

    if (lvgl_port_lock(500))
    {
        lv_label_set_text(ui_ctx.track_label, name);
        lv_label_set_text_fmt(ui_ctx.track_count_label, "%02d / %02d", index, count);

        if (ui_ctx.status_label != NULL)
        {
            lv_label_set_text(ui_ctx.status_label, "Playing");
        }

        set_status_visual_locked(0x00E5FF, 0x0A4B5A);

        fade_in_obj(ui_ctx.track_label, 8, 0);
        fade_in_obj(ui_ctx.track_count_label, 6, 60);

        if (!ui_ctx.eq_anim_active)
        {
            start_eq_animations();
        }

        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGW(TAG, "LVGL lock timeout, skipped track-start UI update");
    }
}

void ui_notify_track_finished(const char *path)
{
    if (path == NULL || ui_ctx.status_label == NULL)
        return;

    char name[128];
    get_display_name(path, name, sizeof(name));

    char status_text[160];
    snprintf(status_text, sizeof(status_text), "Finished: %s", name);

    if (lvgl_port_lock(500))
    {
        lv_label_set_text(ui_ctx.status_label, status_text);
        set_status_visual_locked(0xFFB25E, 0x5A3A10);
        fade_in_obj(ui_ctx.status_label, 4, 0);
        stop_eq_animations();
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGW(TAG, "LVGL lock timeout, skipped track-finished UI update");
    }
}

/*
 * Initialize ST7789 + LVGL port.
 */
void ui_display_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7789 + LVGL");

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 20));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .color_format = LV_COLOR_FORMAT_RGB565,
    };

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(disp == NULL ? ESP_FAIL : ESP_OK);

    if (lvgl_port_lock(1000))
    {
        ui_create();
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG, "Failed to lock LVGL during UI creation");
    }

    ESP_LOGI(TAG, "LVGL UI ready");
}