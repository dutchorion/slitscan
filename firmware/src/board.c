/*
 * Board support for the FANKE FK743M5-XIH6 core board on the slitscan carrier.
 * Pin assignments come from the flash dump of firmware v0.10.24.11 (see PRD).
 */
#include <string.h>
#include "stm32h7xx_hal.h"
#include "board.h"

/* Sensor supplies: both off at boot. PB10 high = 9 V on, PB11 low = 5 V on. */
#define PWR9V_PORT GPIOB
#define PWR9V_PIN  GPIO_PIN_10
#define PWR5V_PORT GPIOB
#define PWR5V_PIN  GPIO_PIN_11

/* The 5 progress LEDs on the back of the carrier, bit0..bit4 */
static GPIO_TypeDef *const carrier_led_port[5] = { GPIOE, GPIOG, GPIOG, GPIOI, GPIOI };
static const uint16_t carrier_led_pin[5] = { GPIO_PIN_6, GPIO_PIN_14, GPIO_PIN_12, GPIO_PIN_7, GPIO_PIN_4 };

static uint32_t reset_flags;

static void clock_config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    const bool rev_v = HAL_GetREVID() >= REV_ID_V;   /* rev V runs 480 MHz, rev Y 400 MHz */

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    HAL_PWREx_ControlVoltageScaling(rev_v ? PWR_REGULATOR_VOLTAGE_SCALE0 : PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
    }

    /* HSE 25 MHz -> PLL1: /5 = 5 MHz, x192 = 960 MHz VCO (x160 = 800 on rev Y) */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 5;
    osc.PLL.PLLN = rev_v ? 192 : 160;
    osc.PLL.PLLP = 2;                  /* SYSCLK 480 / 400 MHz */
    osc.PLL.PLLQ = 10;                 /* 96 / 80 MHz for SDMMC */
    osc.PLL.PLLR = 2;
    osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    osc.PLL.PLLFRACN = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider = RCC_HCLK_DIV2;     /* 240 / 200 MHz AXI, AHB, FMC */
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV2;    /* timers x2 = 240 / 200 MHz */
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) {
        Error_Handler();
    }
}

static void mpu_config(void)
{
    MPU_Region_InitTypeDef r = {0};

    HAL_MPU_Disable();

    /* External SDRAM: normal memory, not cacheable (caches off until DMA code handles them) */
    r.Enable = MPU_REGION_ENABLE;
    r.Number = MPU_REGION_NUMBER0;
    r.BaseAddress = 0xC0000000;
    r.Size = MPU_REGION_SIZE_32MB;
    r.SubRegionDisable = 0;
    r.TypeExtField = MPU_TEX_LEVEL1;
    r.AccessPermission = MPU_REGION_FULL_ACCESS;
    r.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    r.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    r.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&r);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

static void gpio_config(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    /* Sensor supplies off first */
    HAL_GPIO_WritePin(PWR9V_PORT, PWR9V_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PWR5V_PORT, PWR5V_PIN, GPIO_PIN_SET);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = PWR9V_PIN | PWR5V_PIN;
    HAL_GPIO_Init(GPIOB, &g);

    HAL_GPIO_WritePin(LED_BOARD_PORT, LED_BOARD_PIN, GPIO_PIN_SET);
    g.Pin = LED_BOARD_PIN;
    HAL_GPIO_Init(LED_BOARD_PORT, &g);

    for (int i = 0; i < 5; i++) {
        HAL_GPIO_WritePin(carrier_led_port[i], carrier_led_pin[i], GPIO_PIN_RESET);
        g.Pin = carrier_led_pin[i];
        HAL_GPIO_Init(carrier_led_port[i], &g);
    }
}

/*
 * Undo whatever the USB DFU bootloader left behind when it jumps to us
 * (its vector table, enabled interrupts, PLL clocks). Call before HAL_Init().
 */
void board_early_init(void)
{
    __disable_irq();
    SysTick->CTRL = 0;
    for (uint32_t i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }
    SCB->VTOR = FLASH_BANK1_BASE;
    __DSB();
    __ISB();
    HAL_RCC_DeInit();
    __enable_irq();
}

void board_init(void)
{
    reset_flags = RCC->RSR;
    RCC->RSR |= RCC_RSR_RMVF;

    gpio_config();          /* first, so the LEDs work if clock setup fails */
    mpu_config();
    SCB_EnableICache();
    clock_config();

    /* Cycle counter for timing measurements */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;     /* Cortex-M7: unlock DWT, or the counter stays at 0 */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t board_sysclk_hz(void)
{
    return HAL_RCC_GetSysClockFreq();
}

uint32_t board_reset_cause(void)
{
    return reset_flags;
}

/* All reset flags that were set, e.g. "reset-pin software". Flags accumulate until cleared. */
const char *board_reset_cause_str(void)
{
    static char s[80];
    static const struct { uint32_t bit; const char *name; } f[] = {
        { RCC_RSR_PORRSTF, "power-on" },   { RCC_RSR_BORRSTF, "brown-out" },
        { RCC_RSR_PINRSTF, "reset-pin" },  { RCC_RSR_SFTRSTF, "software" },
        { RCC_RSR_IWDG1RSTF, "watchdog" }, { RCC_RSR_WWDG1RSTF, "window-watchdog" },
        { RCC_RSR_LPWRRSTF, "low-power" },
    };

    s[0] = '\0';
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (reset_flags & f[i].bit) {
            if (s[0]) strcat(s, " ");
            strcat(s, f[i].name);
        }
    }
    return s[0] ? s : "unknown";
}

void board_led(bool on)
{
    HAL_GPIO_WritePin(LED_BOARD_PORT, LED_BOARD_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void board_leds_carrier(uint8_t bits)
{
    for (int i = 0; i < 5; i++) {
        HAL_GPIO_WritePin(carrier_led_port[i], carrier_led_pin[i],
                          (bits >> i) & 1 ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

/* Unrecoverable error: all carrier LEDs on, board LED flickers fast. */
void Error_Handler(void)
{
    __disable_irq();
    board_leds_carrier(0x1F);
    while (1) {
        HAL_GPIO_TogglePin(LED_BOARD_PORT, LED_BOARD_PIN);
        for (volatile uint32_t i = 0; i < 2000000; i++) {
        }
    }
}
