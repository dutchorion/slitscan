/* 2.8" ILI9341 TFT, 320x240 landscape, RGB565, on SPI2 */
#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#define LCD_W 320
#define LCD_H 240

/* RGB565 colours */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define LCD_BLACK   0x0000
#define LCD_WHITE   0xFFFF
#define LCD_RED     RGB565(255, 0, 0)
#define LCD_GREEN   RGB565(0, 255, 0)
#define LCD_BLUE    RGB565(0, 0, 255)
#define LCD_YELLOW  RGB565(255, 255, 0)
#define LCD_CYAN    RGB565(0, 255, 255)
#define LCD_GREY    RGB565(96, 96, 96)

bool lcd_init(void);
void lcd_read_id(uint8_t id[4]);      /* raw bytes of command 0xD3; ILI9341 = xx 00 93 41 */
void lcd_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_clear(uint16_t color);
void lcd_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
uint16_t lcd_text_width(const char *s, uint8_t scale);

#endif
