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

/* ST7789 240x280 wiring */
#define LCD_SPI_HOST SPI3_HOST
#define LCD_PIN_SCLK 10
#define LCD_PIN_MOSI 11
#define LCD_PIN_RST 12
#define LCD_PIN_DC 13
#define LCD_PIN_CS 14
#define LCD_H_RES 240
#define LCD_V_RES 280
#define LCD_BIT_PER_PIXEL 16
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#if LV_VERSION_MAJOR >= 9
#define GET_ACTIVE_SCREEN() lv_screen_active()
#define LV_LABEL_LONG_SCROLL_CIRC_COMPAT LV_LABEL_LONG_SCROLL_CIRCULAR
#else
#define GET_ACTIVE_SCREEN() lv_scr_act()
#define LV_LABEL_LONG_SCROLL_CIRC_COMPAT LV_LABEL_LONG_SCROLL_CIRCULAR
#endif

/* Color Palette (Solid colors to prevent RGB565 banding) */
#define COLOR_BG 0x08090C
#define COLOR_CARD 0x121620
#define COLOR_PRIMARY 0xF2F6FF
#define COLOR_SECONDARY 0x8A93A6
#define COLOR_ACCENT 0x00D5FF
#define COLOR_BORDER 0x263143
#define COLOR_ERROR 0xFF4B7E

/*
 * =====================================================================
 * Visualizer Configuration Macros
 * Adjust these to fine-tune the EQ appearance and behavior.
 * =====================================================================
 */
#define UI_EQ_BAR_COUNT 7                 // Number of EQ bars
#define UI_EQ_BAR_WIDTH 8                 // Width of each bar in pixels
#define UI_EQ_BAR_GAP 5                   // Gap between bars in pixels
#define UI_EQ_BASE_HEIGHT 6               // Minimum/Idle height of bars
#define UI_EQ_BASELINE_Y (LCD_V_RES - 36) // Y coordinate of the bottom of the bars

#define UI_EQ_HEIGHT_MIN 14    // Minimum animated height
#define UI_EQ_HEIGHT_MAX 42    // Maximum animated height
#define UI_EQ_DURATION_MIN 380 // Minimum animation duration (ms)
#define UI_EQ_DURATION_MAX 620 // Maximum animation duration (ms)
#define UI_EQ_STAGGER_DELAY 40 // Delay between each bar's animation start (ms)

#define UI_EQ_COLOR_START 0x00FFFF // Hex color for the first bar (e.g., Cyan)
#define UI_EQ_COLOR_END 0xE600B3   // Hex color for the last bar (e.g., Magenta/Purple)
/* ===================================================================== */

typedef struct
{
    lv_obj_t *scr;

    // Artwork
    lv_obj_t *artwork_card;
    lv_obj_t *vinyl_outer;
    lv_obj_t *vinyl_inner;

    // Track Info
    lv_obj_t *track_label;
    lv_obj_t *track_count_label;

    // Progress
    lv_obj_t *elapsed_label;
    lv_obj_t *progress_bar;
    lv_obj_t *total_label;

    // Visualizer
    lv_obj_t *eq_bars[UI_EQ_BAR_COUNT];
    bool eq_anim_active;

    // State / Format Indicator
    lv_obj_t *state_label;

    // Timers & State
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

/*
 * Dynamic EQ parameter generators.
 * Uses deterministic pseudo-random distribution to create an organic look.
 */
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

/*
 * Interpolates between two hex colors based on index.
 */
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

static void ui_create(void)
{
    ui_ctx.scr = GET_ACTIVE_SCREEN();
    no_scroll(ui_ctx.scr);
    lv_obj_set_style_bg_color(ui_ctx.scr, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ctx.scr, LV_OPA_COVER, LV_PART_MAIN);

    // Artwork Placeholder (Vinyl)
    ui_ctx.artwork_card = lv_obj_create(ui_ctx.scr);
    no_scroll(ui_ctx.artwork_card);
    lv_obj_set_size(ui_ctx.artwork_card, 100, 100);
    lv_obj_align(ui_ctx.artwork_card, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(ui_ctx.artwork_card, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.artwork_card, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.artwork_card, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.artwork_card, 1, LV_PART_MAIN);

    ui_ctx.vinyl_outer = lv_obj_create(ui_ctx.artwork_card);
    no_scroll(ui_ctx.vinyl_outer);
    lv_obj_set_size(ui_ctx.vinyl_outer, 80, 80);
    lv_obj_center(ui_ctx.vinyl_outer);
    lv_obj_set_style_bg_color(ui_ctx.vinyl_outer, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.vinyl_outer, 40, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_ctx.vinyl_outer, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ctx.vinyl_outer, 1, LV_PART_MAIN);

    ui_ctx.vinyl_inner = lv_obj_create(ui_ctx.vinyl_outer);
    no_scroll(ui_ctx.vinyl_inner);
    lv_obj_set_size(ui_ctx.vinyl_inner, 20, 20);
    lv_obj_center(ui_ctx.vinyl_inner);
    lv_obj_set_style_bg_color(ui_ctx.vinyl_inner, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.vinyl_inner, 10, LV_PART_MAIN);

    // Track Info
    ui_ctx.track_label = lv_label_create(ui_ctx.scr);
    lv_obj_set_width(ui_ctx.track_label, 200);
    lv_label_set_long_mode(ui_ctx.track_label, LV_LABEL_LONG_SCROLL_CIRC_COMPAT);
    lv_obj_set_style_text_align(ui_ctx.track_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ctx.track_label, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_anim_time(ui_ctx.track_label, 10000, LV_PART_MAIN);
    lv_label_set_text(ui_ctx.track_label, "Waiting for track...");
    lv_obj_align(ui_ctx.track_label, LV_ALIGN_TOP_MID, 0, 132);

    ui_ctx.track_count_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.track_count_label, "-- / --");
    lv_obj_set_style_text_color(ui_ctx.track_count_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);
    lv_obj_align(ui_ctx.track_count_label, LV_ALIGN_TOP_MID, 0, 156);

    // Progress
    ui_ctx.elapsed_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.elapsed_label, "00:00");
    lv_obj_set_style_text_color(ui_ctx.elapsed_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);
    lv_obj_set_size(ui_ctx.elapsed_label, 40, LV_SIZE_CONTENT);
    lv_obj_align(ui_ctx.elapsed_label, LV_ALIGN_TOP_LEFT, 20, 184);

    ui_ctx.total_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.total_label, "00:00");
    lv_obj_set_style_text_color(ui_ctx.total_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);
    lv_obj_set_size(ui_ctx.total_label, 40, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(ui_ctx.total_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(ui_ctx.total_label, LV_ALIGN_TOP_RIGHT, -20, 184);

    ui_ctx.progress_bar = lv_bar_create(ui_ctx.scr);
    lv_obj_set_size(ui_ctx.progress_bar, 112, 4);
    lv_obj_align(ui_ctx.progress_bar, LV_ALIGN_TOP_MID, 0, 188);
    lv_obj_set_style_bg_color(ui_ctx.progress_bar, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ctx.progress_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui_ctx.progress_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_ctx.progress_bar, 2, LV_PART_INDICATOR);
    lv_bar_set_value(ui_ctx.progress_bar, 0, LV_ANIM_OFF);

    // Visualizer (Dynamically generated from macros)
    int total_w = UI_EQ_BAR_COUNT * UI_EQ_BAR_WIDTH + (UI_EQ_BAR_COUNT - 1) * UI_EQ_BAR_GAP;
    int start_x = (LCD_H_RES - total_w) / 2;

    for (int i = 0; i < UI_EQ_BAR_COUNT; i++)
    {
        ui_ctx.eq_bars[i] = lv_obj_create(ui_ctx.scr);
        no_scroll(ui_ctx.eq_bars[i]);
        lv_obj_set_size(ui_ctx.eq_bars[i], UI_EQ_BAR_WIDTH, UI_EQ_BASE_HEIGHT);

        // Calculate perfect centering dynamically
        int16_t x_offset = (int16_t)(start_x + i * (UI_EQ_BAR_WIDTH + UI_EQ_BAR_GAP) - (LCD_H_RES / 2));
        lv_obj_align(ui_ctx.eq_bars[i], LV_ALIGN_TOP_MID, x_offset, UI_EQ_BASELINE_Y - UI_EQ_BASE_HEIGHT);

        // Interpolate color dynamically
        uint32_t color = interpolate_color(UI_EQ_COLOR_START, UI_EQ_COLOR_END, i, UI_EQ_BAR_COUNT);
        lv_obj_set_style_bg_color(ui_ctx.eq_bars[i], lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ui_ctx.eq_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(ui_ctx.eq_bars[i], UI_EQ_BAR_WIDTH / 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(ui_ctx.eq_bars[i], 0, LV_PART_MAIN);
    }

    // State / Format Indicator
    ui_ctx.state_label = lv_label_create(ui_ctx.scr);
    lv_label_set_text(ui_ctx.state_label, "● READY");
    lv_obj_set_style_text_color(ui_ctx.state_label, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(ui_ctx.state_label, 1, LV_PART_MAIN);
    lv_obj_align(ui_ctx.state_label, LV_ALIGN_BOTTOM_MID, 0, -12);
}

void ui_set_status(const char *text)
{
    if (!ui_ctx.state_label || !text)
        return;

    const char *state_text = "● READY";
    uint32_t color = COLOR_ACCENT;

    if (strstr(text, "Mount") || strstr(text, "Scanning") || strstr(text, "Starting"))
    {
        state_text = "● LOADING";
    }
    else if (strstr(text, "No PCM") || strstr(text, "Failed") || strstr(text, "Error"))
    {
        state_text = "● ERROR";
        color = COLOR_ERROR;
    }

    if (lvgl_port_lock(500))
    {
        lv_label_set_text(ui_ctx.state_label, state_text);
        lv_obj_set_style_text_color(ui_ctx.state_label, lv_color_hex(color), LV_PART_MAIN);
        lvgl_port_unlock();
    }
}

void ui_notify_track_started(const char *path, int index, int count, uint32_t duration_sec)
{
    if (!path || !ui_ctx.track_label)
        return;

    char name[128];
    get_display_name(path, name, sizeof(name));

    if (lvgl_port_lock(500))
    {
        lv_label_set_text(ui_ctx.track_label, name);
        lv_label_set_text_fmt(ui_ctx.track_count_label, "%02d / %02d", index, count);

        ui_ctx.total_duration_sec = duration_sec;
        ui_ctx.elapsed_duration_sec = 0;

        char buf[16];
        snprintf(buf, sizeof(buf), "%02lu:%02lu", duration_sec / 60, duration_sec % 60);
        lv_label_set_text(ui_ctx.total_label, buf);
        lv_label_set_text(ui_ctx.elapsed_label, "00:00");
        lv_bar_set_value(ui_ctx.progress_bar, 0, LV_ANIM_OFF);

        if (ui_ctx.progress_timer)
            lv_timer_reset(ui_ctx.progress_timer);

        lv_label_set_text(ui_ctx.state_label, "PCM 44.1kHz");
        lv_obj_set_style_text_color(ui_ctx.state_label, lv_color_hex(COLOR_SECONDARY), LV_PART_MAIN);

        if (!ui_ctx.eq_anim_active)
            start_eq_animations();

        lvgl_port_unlock();
    }
}

void ui_notify_track_finished(const char *path)
{
    // Seamlessly transition to the next track without stopping the visualizer
}

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
        ui_ctx.progress_timer = lv_timer_create(progress_timer_cb, 1000, NULL);
        lvgl_port_unlock();
    }
    else
    {
        ESP_LOGE(TAG, "Failed to lock LVGL during UI creation");
    }
    ESP_LOGI(TAG, "LVGL UI ready");
}