/*
 * FMC SDRAM driver for the W9825G6KH-6I (4 banks x 8192 rows x 512 columns x 16 bit).
 * FMC kernel clock = HCLK (240 MHz), SDCLK = HCLK/2 = 120 MHz (8.33 ns).
 */
#include "stm32h7xx_hal.h"
#include "board.h"
#include "sdram.h"

static SDRAM_HandleTypeDef hsdram;

static void sdram_gpio(void)
{
    GPIO_InitTypeDef g = {0};

    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF12_FMC;

    g.Pin = GPIO_PIN_0;                                               /* SDNWE */
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
            GPIO_PIN_14 | GPIO_PIN_15;                                /* D0-D3, D13-D15 */
    HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
            GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;  /* NBL0-1, D4-D12 */
    HAL_GPIO_Init(GPIOE, &g);
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 |
            GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;  /* A0-A9, SDNRAS */
    HAL_GPIO_Init(GPIOF, &g);
    g.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_5 |
            GPIO_PIN_8 | GPIO_PIN_15;                                 /* A10-A12, BA0-1, SDCLK, SDNCAS */
    HAL_GPIO_Init(GPIOG, &g);
    g.Pin = GPIO_PIN_2 | GPIO_PIN_3;                                  /* SDCKE0, SDNE0 */
    HAL_GPIO_Init(GPIOH, &g);
}

static bool sdram_cmd(uint32_t mode, uint32_t refresh, uint32_t mrd)
{
    FMC_SDRAM_CommandTypeDef c = {0};

    c.CommandMode = mode;
    c.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
    c.AutoRefreshNumber = refresh;
    c.ModeRegisterDefinition = mrd;
    return HAL_SDRAM_SendCommand(&hsdram, &c, 0xFFFF) == HAL_OK;
}

bool sdram_init(void)
{
    RCC_PeriphCLKInitTypeDef pclk = {0};
    FMC_SDRAM_TimingTypeDef t = {0};

    pclk.PeriphClockSelection = RCC_PERIPHCLK_FMC;
    pclk.FmcClockSelection = RCC_FMCCLKSOURCE_D1HCLK;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        return false;
    }
    __HAL_RCC_FMC_CLK_ENABLE();
    sdram_gpio();

    hsdram.Instance = FMC_SDRAM_DEVICE;
    hsdram.Init.SDBank = FMC_SDRAM_BANK1;
    hsdram.Init.ColumnBitsNumber = FMC_SDRAM_COLUMN_BITS_NUM_9;
    hsdram.Init.RowBitsNumber = FMC_SDRAM_ROW_BITS_NUM_13;
    hsdram.Init.MemoryDataWidth = FMC_SDRAM_MEM_BUS_WIDTH_16;
    hsdram.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
    hsdram.Init.CASLatency = FMC_SDRAM_CAS_LATENCY_3;
    hsdram.Init.WriteProtection = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
    hsdram.Init.SDClockPeriod = FMC_SDRAM_CLOCK_PERIOD_2;
    hsdram.Init.ReadBurst = FMC_SDRAM_RBURST_ENABLE;
    hsdram.Init.ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_1;

    /* Datasheet minimums at 8.33 ns/cycle, rounded up */
    t.LoadToActiveDelay = 2;       /* tMRD 2 clk */
    t.ExitSelfRefreshDelay = 9;    /* tXSR 72 ns */
    t.SelfRefreshTime = 6;         /* tRAS 42 ns */
    t.RowCycleDelay = 8;           /* tRC 60 ns */
    t.WriteRecoveryTime = 4;       /* tWR 2 clk; FMC also needs >= tRC - tRCD - tRP */
    t.RPDelay = 2;                 /* tRP 15 ns */
    t.RCDDelay = 2;                /* tRCD 15 ns */

    if (HAL_SDRAM_Init(&hsdram, &t) != HAL_OK) {
        return false;
    }

    /* JEDEC power-up sequence */
    if (!sdram_cmd(FMC_SDRAM_CMD_CLK_ENABLE, 1, 0)) return false;
    HAL_Delay(1);                                   /* >= 100 us */
    if (!sdram_cmd(FMC_SDRAM_CMD_PALL, 1, 0)) return false;
    if (!sdram_cmd(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8, 0)) return false;
    /* Mode: burst length 1, sequential, CAS 3, single-location write burst */
    if (!sdram_cmd(FMC_SDRAM_CMD_LOAD_MODE, 1, (3 << 4) | (1 << 9))) return false;

    /* 64 ms / 8192 rows = 7.81 us; x 120 MHz = 937, minus the 20-cycle safety margin */
    return HAL_SDRAM_ProgramRefreshRate(&hsdram, 917) == HAL_OK;
}

/* ---------------------------------------------------------------- memory test */

static uint32_t xorshift(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static bool fail(sdram_test_t *res, const char *stage, volatile uint32_t *addr, uint32_t exp, uint32_t got)
{
    res->ok = false;
    res->fail_stage = stage;
    res->fail_addr = (uint32_t)addr;
    res->fail_expected = exp;
    res->fail_read = got;
    return false;
}

static bool test_data_bus(sdram_test_t *res)
{
    volatile uint32_t *p = (volatile uint32_t *)SDRAM_BASE;

    for (uint32_t pattern = 1; pattern != 0; pattern <<= 1) {
        *p = pattern;
        __DSB();
        if (*p != pattern) {
            return fail(res, "data bus", p, pattern, *p);
        }
    }
    return true;
}

static bool test_address_bus(sdram_test_t *res)
{
    volatile uint32_t *base = (volatile uint32_t *)SDRAM_BASE;
    const uint32_t words = SDRAM_SIZE / 4;

    for (uint32_t off = 1; off < words; off <<= 1) {
        base[off] = 0xAAAAAAAA;
    }
    base[0] = 0x55555555;
    for (uint32_t off = 1; off < words; off <<= 1) {
        if (base[off] != 0xAAAAAAAA) {
            return fail(res, "address bus (stuck high)", &base[off], 0xAAAAAAAA, base[off]);
        }
    }
    base[0] = 0xAAAAAAAA;
    for (uint32_t test = 1; test < words; test <<= 1) {
        base[test] = 0x55555555;
        if (base[0] != 0xAAAAAAAA) {
            return fail(res, "address bus (short)", &base[test], 0xAAAAAAAA, base[0]);
        }
        for (uint32_t off = 1; off < words; off <<= 1) {
            if (off != test && base[off] != 0xAAAAAAAA) {
                return fail(res, "address bus (stuck low)", &base[test], 0xAAAAAAAA, base[off]);
            }
        }
        base[test] = 0xAAAAAAAA;
    }
    return true;
}

/* Fill all 32 MB with a pseudo-random sequence, then verify; repeated with the bits inverted. */
static bool test_full(sdram_test_t *res)
{
    volatile uint32_t *p = (volatile uint32_t *)SDRAM_BASE;
    const uint32_t words = SDRAM_SIZE / 4;
    const uint32_t mhz = board_sysclk_hz() / 1000000;

    for (int pass = 0; pass < 2; pass++) {
        const uint32_t inv = pass ? 0xFFFFFFFF : 0;
        uint32_t x = 0x12345678;
        uint32_t t0 = board_cycles();
        for (uint32_t i = 0; i < words; i++) {
            x = xorshift(x);
            p[i] = x ^ inv;
        }
        uint32_t t1 = board_cycles();
        x = 0x12345678;
        for (uint32_t i = 0; i < words; i++) {
            x = xorshift(x);
            uint32_t got = p[i];
            if (got != (x ^ inv)) {
                return fail(res, inv ? "full (inverted)" : "full", &p[i], x ^ inv, got);
            }
        }
        uint32_t t2 = board_cycles();
        if (pass == 0) {
            /* KB/s = bytes * MHz * 1e6 / (cycles * 1024); includes the pattern generation */
            res->write_kbps = (uint32_t)((uint64_t)SDRAM_SIZE * mhz * 1000000 / 1024 / (t1 - t0));
            res->read_kbps = (uint32_t)((uint64_t)SDRAM_SIZE * mhz * 1000000 / 1024 / (t2 - t1));
        }
    }
    return true;
}

void sdram_test(sdram_test_t *res)
{
    *res = (sdram_test_t){ .ok = true, .fail_stage = "" };
    if (test_data_bus(res) && test_address_bus(res) && test_full(res)) {
        res->ok = true;
    }
}
