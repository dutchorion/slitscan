/* microSD card on SDMMC1 (4-bit), card detect on PD5 */
#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t blocks;       /* 512-byte blocks */
    uint32_t card_type;    /* HAL CARD_SDSC / CARD_SDHC_SDXC */
    uint32_t clock_hz;
    bool     high_speed;
} sdcard_info_t;

bool sdcard_init(void);
bool sdcard_present(void);
bool sdcard_ready(void);
bool sdcard_set_high_speed(bool on);   /* 48 MHz if the card supports it, else 24 MHz */
void sdcard_info(sdcard_info_t *info);
bool sdcard_read(uint8_t *buf, uint32_t block, uint32_t count);
bool sdcard_write(const uint8_t *buf, uint32_t block, uint32_t count);
bool sdcard_sync(void);    /* wait until the card has finished programming */

#endif
