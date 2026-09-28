/*
 * Slitscan camera firmware v2
 *
 * Milestone M1: board bring-up. Sets full clock speed, tests the 32 MB SDRAM,
 * starts the RTC without losing its time, mounts the SD card, measures card
 * speed and appends a report to LOG.TXT.
 *
 * Carrier LEDs while running: the step number (1..6).
 * Carrier LEDs when done: bit0 SDRAM ok, bit1 clock ok, bit2 card mounted,
 *   bit3 log written, bit4 card fast enough for recording.
 * Board LED when done: slow blink = all passed, fast blink = something failed.
 */
#include <string.h>
#include "stm32h7xx_hal.h"
#include "board.h"
#include "sdram.h"
#include "rtclock.h"
#include "sdcard.h"
#include "log.h"
#include "ff.h"

const char fw_version[] = "slitscan " FW_VERSION;

/* Recording at the fastest line rate needs ~3.7 MB/s; ask for margin. */
#define REQUIRED_WRITE_KBPS  (6 * 1024)

#define SPEED_FILE     "SPEEDTST.BIN"
#define SPEED_BYTES    (16UL * 1024 * 1024)
#define SPEED_CHUNK    (64UL * 1024)
#define SPEED_SRC      ((uint8_t *)SDRAM_BASE)                    /* 1 MB of test pattern */
#define SPEED_DST      ((uint8_t *)(SDRAM_BASE + 16UL * 1024 * 1024))

static FATFS fs;
static FIL file;

typedef struct {
    bool ok;
    const char *error;
    uint32_t write_kbps, read_kbps;
    uint32_t max_chunk_ms;
} speed_t;

/* Print a KB/s value as MB/s with two decimals (no float printf in newlib-nano) */
static void log_mbps(const char *label, uint32_t kbps)
{
    uint32_t hundredths = kbps * 100 / 1024;
    log_printf("%s%lu.%02lu MB/s", label, hundredths / 100, hundredths % 100);
}

static void log_time_now(void)
{
    rtclock_time_t t;
    if (rtclock_get(&t)) {
        log_printf("%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour, t.min, t.sec);
    } else {
        log_printf("(clock unreadable)");
    }
}

static void speed_test(speed_t *r)
{
    UINT n;
    uint32_t chunks = SPEED_BYTES / SPEED_CHUNK;

    memset(r, 0, sizeof(*r));
    if (f_open(&file, SPEED_FILE, FA_CREATE_ALWAYS | FA_WRITE | FA_READ) != FR_OK) {
        r->error = "cannot create test file";
        return;
    }
    /* Pre-allocate a contiguous area, as the recorder will */
    if (f_expand(&file, SPEED_BYTES, 1) != FR_OK) {
        r->error = "not enough contiguous free space (16 MB)";
        f_close(&file);
        f_unlink(SPEED_FILE);
        return;
    }

    uint32_t t0 = HAL_GetTick();
    for (uint32_t i = 0; i < chunks; i++) {
        uint32_t c0 = HAL_GetTick();
        if (f_write(&file, SPEED_SRC + (i % 16) * SPEED_CHUNK, SPEED_CHUNK, &n) != FR_OK || n != SPEED_CHUNK) {
            r->error = "write failed";
            goto out;
        }
        uint32_t dt = HAL_GetTick() - c0;
        if (dt > r->max_chunk_ms) r->max_chunk_ms = dt;
    }
    if (f_sync(&file) != FR_OK) {
        r->error = "sync failed";
        goto out;
    }
    uint32_t t_write = HAL_GetTick() - t0;

    f_lseek(&file, 0);
    t0 = HAL_GetTick();
    for (uint32_t i = 0; i < chunks; i++) {
        if (f_read(&file, SPEED_DST, SPEED_CHUNK, &n) != FR_OK || n != SPEED_CHUNK) {
            r->error = "read failed";
            goto out;
        }
        if (memcmp(SPEED_DST, SPEED_SRC + (i % 16) * SPEED_CHUNK, SPEED_CHUNK) != 0) {
            r->error = "data read back does not match";
            goto out;
        }
    }
    uint32_t t_read = HAL_GetTick() - t0;

    r->write_kbps = t_write ? (uint32_t)((uint64_t)SPEED_BYTES * 1000 / 1024 / t_write) : 0;
    r->read_kbps = t_read ? (uint32_t)((uint64_t)SPEED_BYTES * 1000 / 1024 / t_read) : 0;
    r->ok = true;
out:
    f_close(&file);
    f_unlink(SPEED_FILE);
}

static void log_speed(const char *label, const speed_t *s)
{
    if (!s->ok) {
        log_printf("  %s: FAILED (%s)\r\n", label, s->error);
        return;
    }
    log_printf("  %s: ", label);
    log_mbps("write ", s->write_kbps);
    log_mbps(", read+verify ", s->read_kbps);
    log_printf(", slowest 64 KB write %lu ms\r\n", s->max_chunk_ms);
}

static void blink_forever(bool all_ok)
{
    const uint32_t half_period = all_ok ? 500 : 100;
    while (1) {
        board_led(true);
        HAL_Delay(half_period);
        board_led(false);
        HAL_Delay(half_period);
    }
}

int main(void)
{
    uint8_t result = 0;
    bool log_ok = false, mounted = false;

    board_early_init();
    HAL_Init();
    board_init();
    board_led(true);

    /* LED check: light each carrier LED alone, bit0 first */
    for (int i = 0; i < 5; i++) {
        board_leds_carrier((uint8_t)(1 << i));
        HAL_Delay(400);
    }

    /* 1: clock */
    board_leds_carrier(1);
    rtclock_status_t rtc = rtclock_init();
    if (rtc == RTCLOCK_KEPT || rtc == RTCLOCK_SET_DEFAULT) result |= 1 << 1;

    log_printf("\r\n===== %s boot at ", fw_version);
    log_time_now();
    log_printf(" =====\r\n");
    log_printf("Built:        %s %s\r\n", __DATE__, __TIME__);
    log_printf("CPU:          STM32H7 device 0x%03lx rev 0x%04lx (%s), %lu MHz, flash %u KB\r\n",
               HAL_GetDEVID(), HAL_GetREVID(), HAL_GetREVID() >= REV_ID_V ? "V" : "Y",
               board_sysclk_hz() / 1000000, *(uint16_t *)FLASHSIZE_BASE);
    log_printf("Reset cause:  %s (RSR 0x%08lx)\r\n", board_reset_cause_str(), board_reset_cause());
    log_printf("Clock:        %s\r\n", rtclock_status_str(rtc));

    /* 2: SDRAM */
    board_leds_carrier(2);
    if (!sdram_init()) {
        log_printf("SDRAM:        FAILED to initialise\r\n");
    } else {
        sdram_test_t st;
        uint32_t t0 = HAL_GetTick();
        sdram_test(&st);
        uint32_t dt = HAL_GetTick() - t0;
        if (st.ok) {
            result |= 1 << 0;
            log_printf("SDRAM:        32 MB OK in %lu ms (", dt);
            log_mbps("fill ", st.write_kbps);
            log_mbps(", verify ", st.read_kbps);
            log_printf(")\r\n");
        } else {
            log_printf("SDRAM:        FAILED at %s, address 0x%08lx: wrote 0x%08lx, read 0x%08lx\r\n",
                       st.fail_stage, st.fail_addr, st.fail_expected, st.fail_read);
        }
    }

    /* 3: SD card */
    board_leds_carrier(3);
    if (!sdcard_init()) {
        log_printf("SD card:      not found or init failed (detect pin %s)\r\n", sdcard_present() ? "high" : "low");
    } else {
        sdcard_info_t ci;
        sdcard_info(&ci);
        log_printf("SD card:      %s, %lu MB, bus 4-bit %lu MHz, detect pin %s\r\n",
                   ci.card_type == CARD_SDHC_SDXC ? "SDHC/SDXC" : "SDSC",
                   ci.blocks / 2048, ci.clock_hz / 1000000, sdcard_present() ? "high" : "low");
        FRESULT fr = f_mount(&fs, "", 1);
        if (fr != FR_OK) {
            log_printf("File system:  mount failed (FatFs error %d)\r\n", fr);
        } else {
            FATFS *pfs;
            DWORD free_clust;
            mounted = true;
            result |= 1 << 2;
            if (f_getfree("", &free_clust, &pfs) == FR_OK) {
                uint64_t free_mb = (uint64_t)free_clust * pfs->csize / 2048;
                log_printf("File system:  %s, %lu MB free, cluster %lu KB\r\n",
                           pfs->fs_type == FS_EXFAT ? "exFAT" : pfs->fs_type == FS_FAT32 ? "FAT32" : "FAT",
                           (uint32_t)free_mb, (uint32_t)pfs->csize / 2);
            }
            log_ok = log_flush();     /* save what we have before the speed test */
        }
    }

    /* 4-5: card speed at 24 MHz, then 48 MHz */
    if (mounted && (result & 1)) {
        speed_t s24, s48;

        for (uint32_t i = 0; i < 16 * SPEED_CHUNK / 4; i++) {
            ((uint32_t *)SPEED_SRC)[i] = i * 2654435761u;
        }
        board_leds_carrier(4);
        log_printf("Card speed (16 MB file, 64 KB writes):\r\n");
        speed_test(&s24);
        log_speed("24 MHz", &s24);

        board_leds_carrier(5);
        speed_t *best = &s24;
        if (!sdcard_set_high_speed(true)) {
            log_printf("  48 MHz: card does not support high-speed mode\r\n");
        } else {
            speed_test(&s48);
            log_speed("48 MHz", &s48);
            if (s48.ok && s48.write_kbps > s24.write_kbps) {
                best = &s48;
            } else {
                sdcard_set_high_speed(false);
                log_printf("  staying at 24 MHz\r\n");
            }
        }
        if (best->ok && best->write_kbps >= REQUIRED_WRITE_KBPS) {
            result |= 1 << 4;
            log_printf("  fast enough for recording\r\n");
        } else {
            log_printf("  TOO SLOW for the fastest line rates (need 6 MB/s)\r\n");
        }
    } else if (mounted) {
        log_printf("Card speed:   skipped (needs working SDRAM)\r\n");
    }

    /* 6: write the report */
    board_leds_carrier(6);
    log_printf("Result:       %s\r\n", (result | (1 << 3)) == 0x1F ? "ALL PASSED" : "SOME TESTS FAILED");
    if (mounted) {
        log_ok = log_flush();
    }
    if (log_ok) result |= 1 << 3;

    board_leds_carrier(result);
    blink_forever(result == 0x1F);
}
