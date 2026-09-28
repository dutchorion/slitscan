/*
 * Slitscan camera firmware v2
 *
 * Milestone M3: sensor live view (no recording yet).
 * Powers the ILX514, runs the capture chain taken from the original firmware
 * and shows the current line as a waveform, with statistics and a histogram.
 * Buttons: line time -/+, SAVE (writes the current line to LINEnnn.CSV).
 */
#include <stdio.h>
#include <string.h>
#include "stm32h7xx_hal.h"
#include "board.h"
#include "sdram.h"
#include "rtclock.h"
#include "sdcard.h"
#include "log.h"
#include "lcd.h"
#include "touch.h"
#include "sensor.h"
#include "ff.h"

const char fw_version[] = "slitscan " FW_VERSION;

static FATFS fs;
static FIL file;
static bool mounted;
static uint16_t line[SENSOR_SAMPLES];

/* Line time presets in us; 7000 is the original firmware's default */
static const uint32_t presets[] = { 2500, 3000, 4000, 5000, 7000, 10000, 14000, 20000, 30000 };
#define N_PRESETS (sizeof(presets) / sizeof(presets[0]))
static unsigned preset = 4;

/* Screen layout (top rows hide under the bezel, so start at y = 4) */
#define WAVE_Y     22
#define WAVE_H     120
#define STATS_Y    (WAVE_Y + WAVE_H + 4)
#define HIST_Y     170
#define HIST_H     30
#define BTN_Y      206
#define BTN_H      32
#define WAVE_BG    RGB565(20, 20, 40)

typedef struct { uint16_t x, w; const char *label; } button_t;
static const button_t btn_minus = { 4, 70, "-" };
static const button_t btn_save  = { 118, 84, "SAVE" };
static const button_t btn_plus  = { 246, 70, "+" };

typedef struct {
    uint16_t min, max, mean, black;
    uint32_t clipped;    /* active pixels at 4095 */
} line_stats_t;

/* ------------------------------------------------------------ drawing */

static void draw_button(const button_t *b, uint16_t color)
{
    lcd_fill(b->x, BTN_Y, b->w, BTN_H, color);
    uint16_t tw = lcd_text_width(b->label, 2);
    lcd_text((uint16_t)(b->x + (b->w - tw) / 2), BTN_Y + 6, b->label, LCD_WHITE, color, 2);
}

static bool hit(const button_t *b, int16_t x, int16_t y)
{
    return x >= b->x && x < b->x + b->w && y >= BTN_Y && y < BTN_Y + BTN_H;
}

static void draw_static(void)
{
    lcd_clear(LCD_BLACK);
    draw_button(&btn_minus, LCD_GREY);
    draw_button(&btn_save, RGB565(0, 90, 160));
    draw_button(&btn_plus, LCD_GREY);
}

static void draw_top(uint32_t lines_per_s, uint32_t sync_errors)
{
    char s[48];
    uint32_t us = sensor_line_us();
    snprintf(s, sizeof(s), "LIVE  %lu.%lu ms  %3lu lines/s  sync err %lu   ",
             us / 1000, (us % 1000) / 100, lines_per_s, sync_errors);
    lcd_text(4, 6, s, LCD_WHITE, LCD_BLACK, 1);
}

static uint16_t wave_y(uint16_t v)
{
    return (uint16_t)(WAVE_Y + WAVE_H - 1 - (uint32_t)v * (WAVE_H - 1) / 4095);
}

/* One screen column per 12.8 samples: draw the min..max range of each column */
static void draw_wave(const uint16_t *d)
{
    for (uint16_t x = 0; x < LCD_W; x++) {
        uint32_t a = (uint32_t)x * SENSOR_SAMPLES / LCD_W;
        uint32_t b = (uint32_t)(x + 1) * SENSOR_SAMPLES / LCD_W;
        uint16_t lo = 4095, hi = 0;
        for (uint32_t i = a; i < b; i++) {
            if (d[i] < lo) lo = d[i];
            if (d[i] > hi) hi = d[i];
        }
        bool black = b <= SENSOR_BLACK_LAST + 1;
        bool active = a >= SENSOR_ACTIVE_FIRST && b <= SENSOR_ACTIVE_LAST + 1;
        uint16_t color = black ? LCD_CYAN : active ? LCD_YELLOW : LCD_GREY;
        uint16_t y_hi = wave_y(hi), y_lo = wave_y(lo);
        lcd_fill(x, WAVE_Y, 1, WAVE_H, WAVE_BG);
        lcd_fill(x, y_hi, 1, (uint16_t)(y_lo - y_hi + 1), color);
    }
}

static void compute_stats(const uint16_t *d, line_stats_t *st, uint16_t hist[128])
{
    uint32_t sum = 0, n = 0, bsum = 0;

    memset(hist, 0, 128 * sizeof(uint16_t));
    st->min = 4095;
    st->max = 0;
    st->clipped = 0;
    for (uint32_t i = SENSOR_ACTIVE_FIRST; i <= SENSOR_ACTIVE_LAST; i++) {
        uint16_t v = d[i];
        if (v < st->min) st->min = v;
        if (v > st->max) st->max = v;
        if (v >= 4095) st->clipped++;
        sum += v;
        n++;
        hist[v >> 5]++;
    }
    for (uint32_t i = SENSOR_BLACK_FIRST; i <= SENSOR_BLACK_LAST; i++) {
        bsum += d[i];
    }
    st->mean = (uint16_t)(sum / n);
    st->black = (uint16_t)(bsum / (SENSOR_BLACK_LAST - SENSOR_BLACK_FIRST + 1));
}

static void draw_stats(const line_stats_t *st, const uint16_t hist[128])
{
    char s[64];
    snprintf(s, sizeof(s), "min %4u  max %4u  mean %4u  black %4u  ", st->min, st->max, st->mean, st->black);
    lcd_text(4, STATS_Y, s, LCD_WHITE, LCD_BLACK, 1);
    snprintf(s, sizeof(s), "clipped %lu px   ", st->clipped);
    lcd_text(4, STATS_Y + 12, s, st->clipped ? LCD_RED : LCD_GREY, LCD_BLACK, 1);

    uint16_t peak = 1;
    for (int i = 0; i < 128; i++) if (hist[i] > peak) peak = hist[i];
    for (int i = 0; i < 128; i++) {
        uint16_t h = (uint16_t)((uint32_t)hist[i] * HIST_H / peak);
        uint16_t x = (uint16_t)(32 + i * 2);
        lcd_fill(x, HIST_Y, 2, (uint16_t)(HIST_H - h), LCD_BLACK);
        if (h) lcd_fill(x, (uint16_t)(HIST_Y + HIST_H - h), 2, h, LCD_GREEN);
    }
}

/* ------------------------------------------------------------ saving */

static void save_line(const uint16_t *d, const line_stats_t *st)
{
    char name[16];
    static unsigned n;

    if (!mounted) {
        lcd_text(4, STATS_Y + 12, "no SD card        ", LCD_RED, LCD_BLACK, 1);
        return;
    }
    for (n = n ? n : 1; n < 1000; n++) {
        snprintf(name, sizeof(name), "LINE%03u.CSV", n);
        if (f_open(&file, name, FA_CREATE_NEW | FA_WRITE) == FR_OK) break;
    }
    if (n >= 1000) return;
    f_printf(&file, "# %s, line time %lu us, pixel 500 ns, 12-bit ADC\n", fw_version, sensor_line_us());
    f_printf(&file, "sample,value\n");
    for (int i = 0; i < SENSOR_SAMPLES; i++) {
        f_printf(&file, "%d,%u\n", i, d[i]);
    }
    f_close(&file);
    log_printf("Saved %s: line %lu us, min %u max %u mean %u black %u clipped %lu\r\n",
               name, sensor_line_us(), st->min, st->max, st->mean, st->black, st->clipped);
    log_flush();
    char s[32];
    snprintf(s, sizeof(s), "saved %s   ", name);
    lcd_text(4, STATS_Y + 12, s, LCD_GREEN, LCD_BLACK, 1);
    n++;
}

/* ------------------------------------------------------------ main */

int main(void)
{
    board_early_init();
    HAL_Init();
    board_init();

    rtclock_status_t rtc = rtclock_init();
    rtclock_time_t t;
    log_printf("\r\n===== %s boot at ", fw_version);
    if (rtclock_get(&t)) {
        log_printf("%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour, t.min, t.sec);
    }
    log_printf(" =====\r\nReset cause:  %s\r\nClock:        %s\r\n", board_reset_cause_str(), rtclock_status_str(rtc));
    log_printf("SDRAM:        %s\r\n", sdram_init() ? "initialised" : "FAILED to initialise");
    if (sdcard_init() && f_mount(&fs, "", 1) == FR_OK) {
        mounted = true;
        sdcard_set_high_speed(true);
    }
    log_printf("SD card:      %s\r\n", mounted ? "mounted" : "not available");

    lcd_init();
    touch_info_t ti;
    touch_init(&ti);
    lcd_clear(LCD_BLACK);
    lcd_text(20, 100, "Starting sensor...", LCD_WHITE, LCD_BLACK, 2);

    bool ok = sensor_start(presets[preset]);
    if (ok) {
        log_printf("Sensor:       started, line time %lu us\r\n", presets[preset]);
    } else {
        log_printf("Sensor:       FAILED to start at step: %s\r\n", sensor_last_error());
    }
    if (mounted) log_flush();
    if (!ok) {
        lcd_text(20, 130, "Sensor start FAILED", LCD_RED, LCD_BLACK, 2);
        lcd_text(20, 160, sensor_last_error(), LCD_RED, LCD_BLACK, 2);
        while (1) {
            board_led((HAL_GetTick() / 100) & 1);
        }
    }

    draw_static();

    line_stats_t st = {0};
    uint16_t hist[128];
    uint32_t last_draw = 0, last_rate = HAL_GetTick(), last_log = HAL_GetTick();
    uint32_t lines_at_rate = 0, lines_per_s = 0;
    bool touching = false, have_line = false;

    while (1) {
        uint32_t now = HAL_GetTick();
        sensor_stats_t ss;
        sensor_get_stats(&ss);

        if (now - last_rate >= 1000) {
            lines_per_s = (ss.lines - lines_at_rate) * 1000 / (now - last_rate);
            lines_at_rate = ss.lines;
            last_rate = now;
        }

        if (now - last_draw >= 150 && sensor_snapshot(line)) {
            have_line = true;
            compute_stats(line, &st, hist);
            draw_top(lines_per_s, ss.sync_errors);
            draw_wave(line);
            draw_stats(&st, hist);
            last_draw = now;
        }

        if (mounted && now - last_log >= 10000) {
            log_printf("  %lu lines/s, sync errors %lu, DMA errors %lu, min %u max %u mean %u black %u\r\n",
                       lines_per_s, ss.sync_errors, ss.dma_errors, st.min, st.max, st.mean, st.black);
            log_flush();
            last_log = now;
        }

        int16_t x, y;
        bool down = touch_read(&x, &y);
        if (down && !touching) {
            if (hit(&btn_minus, x, y) && preset > 0) {
                sensor_set_line_us(presets[--preset]);
            } else if (hit(&btn_plus, x, y) && preset < N_PRESETS - 1) {
                sensor_set_line_us(presets[++preset]);
            } else if (hit(&btn_save, x, y) && have_line) {
                save_line(line, &st);
            }
        }
        touching = down;

        board_led((now / 500) & 1);
        HAL_Delay(5);
    }
}
