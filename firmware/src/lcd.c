/*
 * ILI9341 driver. Pins from the original firmware:
 *   SPI2 SCK PI1, MISO PI2, MOSI PI3 (AF5); CS PI0; D/C PD3 (low = command); RST PH14.
 * CS is driven as a GPIO so it can stay low across a command and its data or read-back.
 * Blocking transfers for now; DMA comes with the live view.
 */
#include <string.h>
#include "stm32h7xx_hal.h"
#include "lcd.h"
#include "font.h"

#define CS_LOW()   (GPIOI->BSRR = GPIO_PIN_0 << 16)
#define CS_HIGH()  (GPIOI->BSRR = GPIO_PIN_0)
#define DC_CMD()   (GPIOD->BSRR = GPIO_PIN_3 << 16)
#define DC_DATA()  (GPIOD->BSRR = GPIO_PIN_3)
#define RST_LOW()  (GPIOH->BSRR = GPIO_PIN_14 << 16)
#define RST_HIGH() (GPIOH->BSRR = GPIO_PIN_14)

#define SPI_WRITE_PRESCALER SPI_BAUDRATEPRESCALER_8    /* 96 / 8  = 12 MHz */
#define SPI_READ_PRESCALER  SPI_BAUDRATEPRESCALER_16   /* 96 / 16 = 6 MHz */

static SPI_HandleTypeDef hspi;
static uint16_t linebuf[LCD_W];     /* one row of pixels, big-endian RGB565 */

void HAL_SPI_MspInit(SPI_HandleTypeDef *h)
{
    GPIO_InitTypeDef g = {0};
    (void)h;

    __HAL_RCC_SPI2_CLK_ENABLE();
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI2;
    g.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
    HAL_GPIO_Init(GPIOI, &g);
}

static void set_prescaler(uint32_t p)
{
    MODIFY_REG(hspi.Instance->CFG1, SPI_CFG1_MBR, p);
}

static void write_bytes(const uint8_t *d, uint16_t n)
{
    HAL_SPI_Transmit(&hspi, (uint8_t *)d, n, 100);
}

static void cmd(uint8_t c, const uint8_t *data, uint16_t n)
{
    CS_LOW();
    DC_CMD();
    write_bytes(&c, 1);
    if (n) {
        DC_DATA();
        write_bytes(data, n);
    }
    CS_HIGH();
}

#define CMD(c, ...) do { static const uint8_t d_[] = { __VA_ARGS__ }; cmd(c, d_, sizeof(d_)); } while (0)

void lcd_read_id(uint8_t id[4])
{
    uint8_t tx[5] = {0}, rx[5] = {0};
    uint8_t c = 0xD3;

    set_prescaler(SPI_READ_PRESCALER);
    CS_LOW();
    DC_CMD();
    write_bytes(&c, 1);
    DC_DATA();
    HAL_SPI_TransmitReceive(&hspi, tx, rx, 4, 100);
    CS_HIGH();
    set_prescaler(SPI_WRITE_PRESCALER);
    memcpy(id, rx, 4);
}

bool lcd_init(void)
{
    GPIO_InitTypeDef g = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};

    pclk.PeriphClockSelection = RCC_PERIPHCLK_SPI123;
    pclk.Spi123ClockSelection = RCC_SPI123CLKSOURCE_PLL;      /* PLL1Q = 96 MHz */
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        return false;
    }

    CS_HIGH();
    RST_HIGH();
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOI, &g);
    g.Pin = GPIO_PIN_3;
    HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_14;
    HAL_GPIO_Init(GPIOH, &g);

    hspi.Instance = SPI2;
    hspi.Init.Mode = SPI_MODE_MASTER;
    hspi.Init.Direction = SPI_DIRECTION_2LINES;
    hspi.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi.Init.NSS = SPI_NSS_SOFT;
    hspi.Init.BaudRatePrescaler = SPI_WRITE_PRESCALER;
    hspi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    hspi.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    hspi.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    if (HAL_SPI_Init(&hspi) != HAL_OK) {
        return false;
    }

    /* Hardware reset */
    RST_LOW();
    HAL_Delay(10);
    RST_HIGH();
    HAL_Delay(120);

    /* Standard ILI9341 init; the first part matches the original firmware */
    cmd(0x01, NULL, 0);                            /* software reset */
    HAL_Delay(120);
    CMD(0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02);       /* power control A */
    CMD(0xCF, 0x00, 0xC1, 0x30);                   /* power control B */
    CMD(0xE8, 0x85, 0x00, 0x78);                   /* driver timing A */
    CMD(0xEA, 0x00, 0x00);                         /* driver timing B */
    CMD(0xED, 0x64, 0x03, 0x12, 0x81);             /* power-on sequence */
    CMD(0xF7, 0x20);                               /* pump ratio */
    CMD(0xC0, 0x23);                               /* power control 1 */
    CMD(0xC1, 0x10);                               /* power control 2 */
    CMD(0xC5, 0x3E, 0x28);                         /* VCOM 1 */
    CMD(0xC7, 0x86);                               /* VCOM 2 */
    CMD(0x36, 0x28);                               /* MADCTL: landscape (MV) + BGR */
    CMD(0x3A, 0x55);                               /* 16 bit/pixel */
    CMD(0xB1, 0x00, 0x18);                         /* frame rate 79 Hz */
    CMD(0xB6, 0x08, 0x82, 0x27);                   /* display function */
    CMD(0xF2, 0x00);                               /* 3-gamma off */
    CMD(0x26, 0x01);                               /* gamma curve 1 */
    CMD(0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);
    CMD(0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);
    cmd(0x21, NULL, 0);                            /* inversion on: this panel shows inverted colours without it */
    cmd(0x11, NULL, 0);                            /* sleep out */
    HAL_Delay(120);
    cmd(0x29, NULL, 0);                            /* display on */
    return true;
}

static void set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x1 = x + w - 1, y1 = y + h - 1;
    uint8_t ca[4] = { x >> 8, x & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t ra[4] = { y >> 8, y & 0xFF, y1 >> 8, y1 & 0xFF };

    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
}

/* Send the same row of pixels (already in linebuf) to `rows` rows of the window */
static void push_rows(uint16_t w, uint16_t rows)
{
    uint8_t c = 0x2C;

    CS_LOW();
    DC_CMD();
    write_bytes(&c, 1);
    DC_DATA();
    for (uint16_t r = 0; r < rows; r++) {
        write_bytes((uint8_t *)linebuf, w * 2);
    }
    CS_HIGH();
}

void lcd_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= LCD_W || y >= LCD_H || w == 0 || h == 0) return;
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;

    uint16_t be = (uint16_t)((color >> 8) | (color << 8));
    for (uint16_t i = 0; i < w; i++) linebuf[i] = be;
    set_window(x, y, w, h);
    push_rows(w, h);
}

void lcd_clear(uint16_t color)
{
    lcd_fill(0, 0, LCD_W, LCD_H, color);
}

uint16_t lcd_text_width(const char *s, uint8_t scale)
{
    return (uint16_t)(strlen(s) * (font7x10.width + 1) * scale);
}

/* Draw one character cell (including a 1-pixel gap on the right), row by row */
static void draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg, uint8_t scale)
{
    const font_t *f = &font7x10;
    const uint16_t cw = (uint16_t)((f->width + 1) * scale);
    const uint16_t chh = (uint16_t)(f->height * scale);
    const uint16_t fgb = (uint16_t)((fg >> 8) | (fg << 8));
    const uint16_t bgb = (uint16_t)((bg >> 8) | (bg << 8));

    if (ch < 32 || ch > 126) ch = '?';
    if (x + cw > LCD_W || y + chh > LCD_H) return;

    const uint16_t *glyph = &f->data[(ch - 32) * f->height];
    set_window(x, y, cw, chh);

    uint8_t c = 0x2C;
    CS_LOW();
    DC_CMD();
    write_bytes(&c, 1);
    DC_DATA();
    for (uint16_t row = 0; row < f->height; row++) {
        uint16_t bits = glyph[row];
        for (uint16_t col = 0; col < f->width + 1; col++) {
            uint16_t px = (col < f->width && (bits & (0x8000 >> col))) ? fgb : bgb;
            for (uint8_t s = 0; s < scale; s++) linebuf[col * scale + s] = px;
        }
        for (uint8_t s = 0; s < scale; s++) {
            write_bytes((uint8_t *)linebuf, cw * 2);
        }
    }
    CS_HIGH();
}

void lcd_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if (scale == 0) scale = 1;
    for (; *s; s++) {
        draw_char(x, y, *s, fg, bg, scale);
        x = (uint16_t)(x + (font7x10.width + 1) * scale);
    }
}
