/*
 * Slitscan camera firmware v2
 *
 * Milestone M2: display and touch bring-up.
 *  1. Starts the board (clocks, RTC, SDRAM, SD card log) as in M1.
 *  2. Initialises the ILI9341 and the FT6336 and logs their IDs.
 *  3. Orientation and colour test screen (the backlight is always on).
 *  4. Touch calibration (tap two targets), then a crosshair test.
 */
#include <stdio.h>
#include <stdlib.h>
#include "stm32h7xx_hal.h"
#include "board.h"
#include "sdram.h"
#include "rtclock.h"
#include "sdcard.h"
#include "log.h"
#include "lcd.h"
#include "touch.h"
#include "ff.h"

const char fw_version[] = "slitscan " FW_VERSION;

static FATFS fs;
static bool mounted;

/* ------------------------------------------------------------ orientation test */

/*
 * White frame on the outermost pixels, corner labels, and labelled colour bars.
 * If the frame is fully visible and the labels match their corners, the
 * 320x240 drawing area lines up with the glass.
 */
static void orientation_test(void)
{
    static const struct { uint16_t color; const char *name; } bars[] = {
        { LCD_RED, "RED" }, { LCD_GREEN, "GRN" }, { LCD_BLUE, "BLU" }, { LCD_WHITE, "WHT" },
        { LCD_YELLOW, "YEL" }, { LCD_CYAN, "CYN" }, { LCD_GREY, "GRY" }, { LCD_BLACK, "BLK" },
    };
    uint16_t tx, ty;

    lcd_clear(LCD_BLACK);
    lcd_fill(0, 0, LCD_W, 1, LCD_WHITE);
    lcd_fill(0, LCD_H - 1, LCD_W, 1, LCD_WHITE);
    lcd_fill(0, 0, 1, LCD_H, LCD_WHITE);
    lcd_fill(LCD_W - 1, 0, 1, LCD_H, LCD_WHITE);

    lcd_text(4, 4, "TOP LEFT", LCD_GREEN, LCD_BLACK, 1);
    lcd_text((uint16_t)(LCD_W - 4 - lcd_text_width("TOP RIGHT", 1)), 4, "TOP RIGHT", LCD_GREEN, LCD_BLACK, 1);
    lcd_text(4, LCD_H - 14, "BOTTOM LEFT", LCD_GREEN, LCD_BLACK, 1);
    lcd_text((uint16_t)(LCD_W - 4 - lcd_text_width("BOTTOM RIGHT", 1)), LCD_H - 14, "BOTTOM RIGHT", LCD_GREEN, LCD_BLACK, 1);

    lcd_text(40, 40, "SCREEN TEST", LCD_WHITE, LCD_BLACK, 3);        /* 11 chars x 24 = 264 px */
    lcd_text(40, 80, "Tap to continue", LCD_WHITE, LCD_BLACK, 2);    /* 15 chars x 16 = 240 px */

    for (unsigned i = 0; i < 8; i++) {
        uint16_t x = (uint16_t)(i * 40);
        lcd_fill(x, 150, 40, 50, bars[i].color);
        lcd_text((uint16_t)(x + 8), 205, bars[i].name, LCD_WHITE, LCD_BLACK, 1);
    }

    uint32_t t0 = HAL_GetTick();
    while (!touch_read_raw(&tx, &ty) && HAL_GetTick() - t0 < 60000) {
        HAL_Delay(20);
    }
    while (touch_read_raw(&tx, &ty)) {
        HAL_Delay(20);
    }
}

/* ------------------------------------------------------------ touch */

typedef struct {
    bool swap;                 /* screen X comes from raw y */
    int32_t x0, rx0, x_num, x_den;
    int32_t y0, ry0, y_num, y_den;
} touch_cal_t;

static touch_cal_t cal = {
    /* Measured on the device 2026-09-28: raw (204,45) at (30,30), raw (0,302) at (290,210) */
    .swap = true, .x0 = 30, .rx0 = 45, .x_num = 260, .x_den = 257,
    .y0 = 30, .ry0 = 204, .y_num = 180, .y_den = -204,
};

static void touch_map(uint16_t rx, uint16_t ry, int32_t *sx, int32_t *sy)
{
    int32_t a = cal.swap ? ry : rx;   /* raw value that drives screen X */
    int32_t b = cal.swap ? rx : ry;
    *sx = cal.x0 + (a - cal.rx0) * cal.x_num / cal.x_den;
    *sy = cal.y0 + (b - cal.ry0) * cal.y_num / cal.y_den;
}

/* Wait for a touch, average a few samples, then wait for release. */
static bool wait_tap(uint16_t *rx, uint16_t *ry, uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    uint16_t x, y;

    while (!touch_read_raw(&x, &y)) {
        if (HAL_GetTick() - t0 > timeout_ms) return false;
        HAL_Delay(10);
    }
    uint32_t sx = 0, sy = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        HAL_Delay(15);
        if (touch_read_raw(&x, &y)) { sx += x; sy += y; n++; }
    }
    while (touch_read_raw(&x, &y)) HAL_Delay(10);
    if (n == 0) return false;
    *rx = (uint16_t)(sx / n);
    *ry = (uint16_t)(sy / n);
    return true;
}

static void draw_target(int32_t x, int32_t y, uint16_t color)
{
    lcd_fill((uint16_t)(x - 12), (uint16_t)y, 25, 1, color);
    lcd_fill((uint16_t)x, (uint16_t)(y - 12), 1, 25, color);
    lcd_fill((uint16_t)(x - 3), (uint16_t)(y - 3), 7, 7, color);
}

static void touch_calibrate(void)
{
    const int32_t ax = 30, ay = 30, bx = LCD_W - 30, by = LCD_H - 30;
    uint16_t arx, ary, brx, bry;

    lcd_clear(LCD_BLACK);
    lcd_text(60, 100, "Tap the red target", LCD_WHITE, LCD_BLACK, 2);
    draw_target(ax, ay, LCD_RED);
    if (!wait_tap(&arx, &ary, 30000)) {
        log_printf("Touch calibration: no tap, using default mapping\r\n");
        return;
    }
    draw_target(ax, ay, LCD_BLACK);
    draw_target(bx, by, LCD_RED);
    if (!wait_tap(&brx, &bry, 30000)) {
        log_printf("Touch calibration: no second tap, using default mapping\r\n");
        return;
    }

    /* The raw axis that changed most between the targets follows screen X (the longer distance) */
    int32_t dxr = (int32_t)brx - arx, dyr = (int32_t)bry - ary;
    cal.swap = abs(dyr) > abs(dxr);
    int32_t da = cal.swap ? dyr : dxr;   /* raw change along screen X */
    int32_t db = cal.swap ? dxr : dyr;   /* raw change along screen Y */
    if (da == 0 || db == 0) {
        log_printf("Touch calibration: taps too close, using default mapping\r\n");
        return;
    }
    cal.x0 = ax; cal.rx0 = cal.swap ? ary : arx; cal.x_num = bx - ax; cal.x_den = da;
    cal.y0 = ay; cal.ry0 = cal.swap ? arx : ary; cal.y_num = by - ay; cal.y_den = db;
    log_printf("Touch calibration: raw (%u,%u) at (%ld,%ld), raw (%u,%u) at (%ld,%ld); axes %s, X %s, Y %s\r\n",
               arx, ary, ax, ay, brx, bry, bx, by, cal.swap ? "swapped" : "straight",
               (cal.x_num > 0) == (cal.x_den > 0) ? "normal" : "mirrored",
               (cal.y_num > 0) == (cal.y_den > 0) ? "normal" : "mirrored");
}

static void touch_test(void)
{
    char line[48];
    int32_t lx = -1, ly = -1;
    uint32_t taps = 0, last_flush = HAL_GetTick();
    bool was_down = false;

    lcd_clear(LCD_BLACK);
    lcd_text(10, 8, "TOUCH TEST: draw with a finger", LCD_WHITE, LCD_BLACK, 1);
    lcd_text(10, 22, "Top-left corner is here", LCD_GREY, LCD_BLACK, 1);
    lcd_fill(0, 0, 6, 6, LCD_GREEN);

    while (1) {
        uint16_t rx, ry;
        if (touch_read_raw(&rx, &ry)) {
            int32_t sx, sy;
            touch_map(rx, ry, &sx, &sy);
            if (sx >= 0 && sx < LCD_W && sy >= 0 && sy < LCD_H) {
                if (lx >= 0 && (abs(sx - lx) > 1 || abs(sy - ly) > 1)) {
                    lcd_fill((uint16_t)lx, (uint16_t)ly, 3, 3, LCD_GREY);   /* trail */
                }
                lcd_fill((uint16_t)sx, (uint16_t)sy, 3, 3, LCD_YELLOW);
                lx = sx; ly = sy;
            }
            snprintf(line, sizeof(line), "raw %4u,%4u  screen %4ld,%4ld  ", rx, ry, sx, sy);
            lcd_text(10, LCD_H - 14, line, LCD_CYAN, LCD_BLACK, 1);
            if (!was_down) {
                taps++;
                if (taps <= 20) log_printf("  touch %lu: raw %u,%u -> screen %ld,%ld\r\n", taps, rx, ry, sx, sy);
            }
            was_down = true;
        } else {
            was_down = false;
        }
        if (mounted && HAL_GetTick() - last_flush > 5000) {
            log_flush();
            last_flush = HAL_GetTick();
        }
        HAL_Delay(15);
        board_led((HAL_GetTick() / 500) & 1);
    }
}

/* ------------------------------------------------------------ main */

int main(void)
{
    board_early_init();
    HAL_Init();
    board_init();
    board_leds_carrier(1);

    rtclock_status_t rtc = rtclock_init();
    log_printf("\r\n===== %s boot at ", fw_version);
    rtclock_time_t t;
    if (rtclock_get(&t)) {
        log_printf("%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour, t.min, t.sec);
    }
    log_printf(" =====\r\nReset cause:  %s\r\nClock:        %s\r\n", board_reset_cause_str(), rtclock_status_str(rtc));

    board_leds_carrier(2);
    log_printf("SDRAM:        %s\r\n", sdram_init() ? "initialised" : "FAILED to initialise");

    board_leds_carrier(3);
    if (sdcard_init() && f_mount(&fs, "", 1) == FR_OK) {
        mounted = true;
        sdcard_set_high_speed(true);
        log_printf("SD card:      mounted\r\n");
    }

    board_leds_carrier(4);
    if (!lcd_init()) {
        log_printf("Display:      SPI init FAILED\r\n");
    } else {
        uint8_t id[4];
        lcd_read_id(id);
        log_printf("Display:      ID bytes %02x %02x %02x %02x (ILI9341 = xx 00 93 41)\r\n", id[0], id[1], id[2], id[3]);
    }

    board_leds_carrier(5);
    touch_info_t ti;
    if (touch_init(&ti)) {
        log_printf("Touch:        responds at 0x38; vendor 0x%02x, chip 0x%02x, firmware 0x%02x\r\n",
                   ti.vendor_id, ti.chip_id, ti.fw_version);
    } else {
        log_printf("Touch:        NO RESPONSE at 0x38 (INT pin %s)\r\n", touch_int_active() ? "low" : "high");
    }
    if (mounted) log_flush();

    board_leds_carrier(0);
    orientation_test();
    touch_calibrate();
    if (mounted) log_flush();
    log_printf("Touch test:\r\n");
    touch_test();
}
