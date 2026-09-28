/*
 * FT6336 touch over software I2C, as in the original firmware:
 *   SCL PB6, SDA PB7 (open-drain with pull-ups), INT PD4 (low while touched).
 * About 100 kHz, timed with the DWT cycle counter.
 */
#include "stm32h7xx_hal.h"
#include "board.h"
#include "touch.h"

#define ADDR 0x38

#define SCL_PIN GPIO_PIN_6
#define SDA_PIN GPIO_PIN_7
#define SCL(v)  (GPIOB->BSRR = (v) ? SCL_PIN : (uint32_t)SCL_PIN << 16)
#define SDA(v)  (GPIOB->BSRR = (v) ? SDA_PIN : (uint32_t)SDA_PIN << 16)
#define SDA_IN() ((GPIOB->IDR & SDA_PIN) != 0)

static uint32_t half_bit_cycles;

static void delay_half(void)
{
    uint32_t t0 = board_cycles();
    while (board_cycles() - t0 < half_bit_cycles) {
    }
}

static void i2c_start(void)
{
    SDA(1); SCL(1); delay_half();
    SDA(0); delay_half();
    SCL(0); delay_half();
}

static void i2c_stop(void)
{
    SDA(0); delay_half();
    SCL(1); delay_half();
    SDA(1); delay_half();
}

/* Returns true if the device acknowledged */
static bool i2c_write(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        SDA((b >> i) & 1);
        delay_half();
        SCL(1); delay_half();
        SCL(0);
    }
    SDA(1);                    /* release SDA for ACK */
    delay_half();
    SCL(1); delay_half();
    bool ack = !SDA_IN();
    SCL(0); delay_half();
    return ack;
}

static uint8_t i2c_read(bool ack)
{
    uint8_t b = 0;

    SDA(1);
    for (int i = 7; i >= 0; i--) {
        delay_half();
        SCL(1); delay_half();
        if (SDA_IN()) b |= (uint8_t)(1 << i);
        SCL(0);
    }
    SDA(ack ? 0 : 1);
    delay_half();
    SCL(1); delay_half();
    SCL(0); delay_half();
    SDA(1);
    return b;
}

static bool read_regs(uint8_t reg, uint8_t *buf, int n)
{
    i2c_start();
    if (!i2c_write(ADDR << 1)) { i2c_stop(); return false; }
    if (!i2c_write(reg)) { i2c_stop(); return false; }
    i2c_start();                                    /* repeated start */
    if (!i2c_write((ADDR << 1) | 1)) { i2c_stop(); return false; }
    for (int i = 0; i < n; i++) {
        buf[i] = i2c_read(i < n - 1);
    }
    i2c_stop();
    return true;
}

bool touch_init(touch_info_t *info)
{
    GPIO_InitTypeDef g = {0};

    half_bit_cycles = board_sysclk_hz() / 200000;   /* 5 us: ~100 kHz */

    SCL(1);
    SDA(1);
    g.Pin = SCL_PIN | SDA_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);

    g.Pin = GPIO_PIN_4;                              /* INT */
    g.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOD, &g);

    /* Free a bus that may be stuck mid-transfer: clock out 9 bits, then stop */
    for (int i = 0; i < 9; i++) {
        SCL(0); delay_half();
        SCL(1); delay_half();
    }
    i2c_stop();

    info->responds = read_regs(0xA8, &info->vendor_id, 1);
    if (info->responds) {
        read_regs(0xA3, &info->chip_id, 1);
        read_regs(0xA6, &info->fw_version, 1);
    }
    return info->responds;
}

bool touch_read_raw(uint16_t *x, uint16_t *y)
{
    uint8_t r[5];   /* 0x02 TD_STATUS, 0x03 P1_XH, 0x04 P1_XL, 0x05 P1_YH, 0x06 P1_YL */

    if (!read_regs(0x02, r, 5)) return false;
    if ((r[0] & 0x0F) == 0 || (r[0] & 0x0F) > 2) return false;
    *x = (uint16_t)(((r[1] & 0x0F) << 8) | r[2]);
    *y = (uint16_t)(((r[3] & 0x0F) << 8) | r[4]);
    return true;
}

bool touch_int_active(void)
{
    return HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_4) == GPIO_PIN_RESET;
}

/*
 * Map to landscape screen coordinates. Calibration measured on the device
 * (2026-09-28): raw (204,45) at screen (30,30), raw (0,302) at (290,210).
 * Screen X follows raw y; screen Y follows raw x, mirrored.
 */
bool touch_read(int16_t *sx, int16_t *sy)
{
    uint16_t rx, ry;

    if (!touch_read_raw(&rx, &ry)) return false;
    int32_t x = 30 + ((int32_t)ry - 45) * 260 / 257;
    int32_t y = 30 + ((int32_t)rx - 204) * 180 / -204;
    if (x < 0) x = 0;
    if (x > 319) x = 319;
    if (y < 0) y = 0;
    if (y > 239) y = 239;
    *sx = (int16_t)x;
    *sy = (int16_t)y;
    return true;
}
