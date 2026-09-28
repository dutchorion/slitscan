/* FT6336-family capacitive touch controller, I2C address 0x38 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t vendor_id;   /* reg 0xA8, FocalTech = 0x11 */
    uint8_t chip_id;     /* reg 0xA3, FT6336U = 0x64, FT6236 = 0x36 */
    uint8_t fw_version;  /* reg 0xA6 */
    bool    responds;    /* acknowledged its I2C address */
} touch_info_t;

bool touch_init(touch_info_t *info);
/* Raw panel coordinates of the first touch point; false if not touched */
bool touch_read_raw(uint16_t *x, uint16_t *y);
bool touch_int_active(void);   /* INT pin (PD4) low */
/* Touch position in screen pixels (320x240 landscape); false if not touched */
bool touch_read(int16_t *sx, int16_t *sy);

#endif
