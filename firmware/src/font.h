/* Bitmap fonts: one uint16 per row, MSB = leftmost pixel, ASCII 32..126 */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

typedef struct {
    uint8_t width;
    uint8_t height;
    const uint16_t *data;
} font_t;

extern const font_t font7x10;

#endif
