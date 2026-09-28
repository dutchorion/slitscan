/*
 * microSD driver on SDMMC1 using the controller's internal DMA (IDMA).
 * IDMA reaches AXI SRAM and the FMC (SDRAM) only, not DTCM or D2 SRAM, so other
 * buffers are copied through a bounce buffer. D-cache is off, so no cache maintenance.
 */
#include <string.h>
#include "stm32h7xx_hal.h"
#include "sdcard.h"

#define SD_TIMEOUT_MS   2000
#define BOUNCE_BLOCKS   8

static SD_HandleTypeDef hsd;
static volatile bool xfer_done, xfer_err;
static bool initialised, high_speed;
static uint8_t bounce[BOUNCE_BLOCKS * 512] __attribute__((aligned(32)));   /* in AXI SRAM (.bss) */

void SDMMC1_IRQHandler(void)
{
    HAL_SD_IRQHandler(&hsd);
}

void HAL_SD_TxCpltCallback(SD_HandleTypeDef *h) { (void)h; xfer_done = true; }
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *h) { (void)h; xfer_done = true; }
void HAL_SD_ErrorCallback(SD_HandleTypeDef *h)  { (void)h; xfer_err = true; }

void HAL_SD_MspInit(SD_HandleTypeDef *h)
{
    GPIO_InitTypeDef g = {0};
    (void)h;

    __HAL_RCC_SDMMC1_CLK_ENABLE();
    __HAL_RCC_SDMMC1_FORCE_RESET();
    __HAL_RCC_SDMMC1_RELEASE_RESET();

    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF12_SDMMC1;
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;   /* D0-D3 */
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2;                                            /* CMD */
    HAL_GPIO_Init(GPIOD, &g);
    g.Pull = GPIO_NOPULL;
    g.Pin = GPIO_PIN_12;                                           /* CK */
    HAL_GPIO_Init(GPIOC, &g);

    HAL_NVIC_SetPriority(SDMMC1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
}

bool sdcard_present(void)
{
    /* Card detect switch polarity not yet confirmed on this board; logged, not enforced */
    return HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_5) == GPIO_PIN_SET;
}

bool sdcard_init(void)
{
    RCC_PeriphCLKInitTypeDef pclk = {0};
    GPIO_InitTypeDef g = {0};

    g.Pin = GPIO_PIN_5;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOD, &g);

    pclk.PeriphClockSelection = RCC_PERIPHCLK_SDMMC;
    pclk.SdmmcClockSelection = RCC_SDMMCCLKSOURCE_PLL;          /* PLL1Q = 96 MHz */
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        return false;
    }

    hsd.Instance = SDMMC1;
    hsd.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    hsd.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.BusWide = SDMMC_BUS_WIDE_4B;
    hsd.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
    hsd.Init.ClockDiv = 2;                                      /* 96 / (2*2) = 24 MHz */
    if (HAL_SD_Init(&hsd) != HAL_OK) {
        initialised = false;
        return false;
    }

    high_speed = false;
    initialised = true;
    return true;
}

bool sdcard_set_high_speed(bool on)
{
    if (!initialised) return false;
    if (on) {
        /* Switch the card to SDR25 (50 MHz class), then raise the clock to 96 / 2 = 48 MHz */
        if (HAL_SD_ConfigSpeedBusOperation(&hsd, SDMMC_SPEED_MODE_HIGH) != HAL_OK) {
            hsd.State = HAL_SD_STATE_READY;
            return false;
        }
        MODIFY_REG(hsd.Instance->CLKCR, SDMMC_CLKCR_CLKDIV, 1U);
    } else {
        MODIFY_REG(hsd.Instance->CLKCR, SDMMC_CLKCR_CLKDIV, 2U);
    }
    high_speed = on;
    return true;
}

bool sdcard_ready(void)
{
    return initialised;
}

void sdcard_info(sdcard_info_t *info)
{
    HAL_SD_CardInfoTypeDef ci;

    memset(info, 0, sizeof(*info));
    if (!initialised || HAL_SD_GetCardInfo(&hsd, &ci) != HAL_OK) {
        return;
    }
    info->blocks = ci.LogBlockNbr;
    info->card_type = ci.CardType;
    info->high_speed = high_speed;
    uint32_t div = (hsd.Instance->CLKCR & SDMMC_CLKCR_CLKDIV);
    uint32_t kernel = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SDMMC);
    info->clock_hz = div ? kernel / (2 * div) : kernel;
}

static bool wait_transfer_state(void)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        if (HAL_GetTick() - t0 > SD_TIMEOUT_MS) {
            return false;
        }
    }
    return true;
}

static bool wait_xfer(void)
{
    uint32_t t0 = HAL_GetTick();
    while (!xfer_done && !xfer_err) {
        if (HAL_GetTick() - t0 > SD_TIMEOUT_MS) {
            return false;
        }
    }
    return xfer_done && !xfer_err;
}

static bool dma_reachable(const void *p)
{
    uint32_t a = (uint32_t)p;
    bool axi = a >= 0x24000000 && a < 0x24080000;
    bool fmc = a >= 0xC0000000 && a < 0xC2000000;
    return (axi || fmc) && (a & 3) == 0;
}

static bool xfer(uint8_t *buf, uint32_t block, uint32_t count, bool write)
{
    if (!wait_transfer_state()) {
        return false;
    }
    xfer_done = xfer_err = false;
    HAL_StatusTypeDef st = write ? HAL_SD_WriteBlocks_DMA(&hsd, buf, block, count)
                                 : HAL_SD_ReadBlocks_DMA(&hsd, buf, block, count);
    return st == HAL_OK && wait_xfer();
}

bool sdcard_read(uint8_t *buf, uint32_t block, uint32_t count)
{
    if (!initialised) return false;
    if (dma_reachable(buf)) {
        return xfer(buf, block, count, false);
    }
    while (count) {
        uint32_t n = count > BOUNCE_BLOCKS ? BOUNCE_BLOCKS : count;
        if (!xfer(bounce, block, n, false)) return false;
        memcpy(buf, bounce, n * 512);
        buf += n * 512; block += n; count -= n;
    }
    return true;
}

bool sdcard_write(const uint8_t *buf, uint32_t block, uint32_t count)
{
    if (!initialised) return false;
    if (dma_reachable(buf)) {
        return xfer((uint8_t *)buf, block, count, true);
    }
    while (count) {
        uint32_t n = count > BOUNCE_BLOCKS ? BOUNCE_BLOCKS : count;
        memcpy(bounce, buf, n * 512);
        if (!xfer(bounce, block, n, true)) return false;
        buf += n * 512; block += n; count -= n;
    }
    return true;
}

bool sdcard_sync(void)
{
    return initialised && wait_transfer_state();
}
