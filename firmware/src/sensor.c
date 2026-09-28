/*
 * ILX514 capture chain, reproducing the original firmware (v0.10.24.11) timing:
 *
 *   TIM1 CH1 (PA8)  phiROG: 5 us high at the start of each line; period = line time.
 *                   TRGO = OC1REF, high from 5 us to the end of the line.
 *   TIM3 CH2/3/4    phiCLK: 500 ns period, ~117 ns high (PC7, PB0, PB1).
 *                   Gated by TIM1 TRGO, so it runs outside the ROG pulse.
 *   TIM2 CH2/3      one-pulse, triggered by TIM1 TRGO: high ~4..30 us later (PA1, PA2).
 *                   On the carrier this is wired to PA0 = TIM8_ETR.
 *   TIM8            triggered by the falling edge on ETR; 4096 periods of 500 ns.
 *                   TRGO2 = OC1REF drives the ADC; CH1/CH2 on PC6, PI6 as in the original.
 *   ADC3 IN0 (PC2_C) 12-bit, one conversion per TIM8 period, circular DMA of 2 lines.
 *
 * All values are taken from the original firmware (register dump of v0.10.24.11).
 * It ran its timers at 120 MHz; this build runs them at 240 MHz, so counts are
 * computed from the actual timer clock to give the same times.
 * Sensor supplies: 9 V (PB10 high) before 5 V (PB11 low); off in reverse order.
 * The timer pins are only connected while the sensor is powered.
 */
#include <string.h>
#include "stm32h7xx_hal.h"
#include "sensor.h"
#include "log.h"

#define PIXEL_NS        500
#define ROG_US          5
#define ADC_BUF_LEN     (2 * SENSOR_SAMPLES)

static ADC_HandleTypeDef hadc;
static DMA_HandleTypeDef hdma;
static uint16_t adc_buf[ADC_BUF_LEN] __attribute__((aligned(32)));   /* AXI SRAM: reachable by DMA2 */
static uint16_t snap[SENSOR_SAMPLES];

static volatile bool want_snap, snap_ready;
static volatile uint32_t lines, sync_errors, dma_errors;
static bool running;
static uint32_t line_us;
static uint32_t tim_clk;
static const char *last_error = "";

const char *sensor_last_error(void)
{
    return last_error;
}

/* ------------------------------------------------------------------ power */

static void power_off(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_11, GPIO_PIN_SET);     /* 5 V off */
    HAL_Delay(50);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_RESET);   /* 9 V off */
}

/* ------------------------------------------------------------------ pins */

typedef struct { GPIO_TypeDef *port; uint16_t pin; uint8_t af; } pin_t;

static const pin_t timer_pins[] = {
    { GPIOA, GPIO_PIN_8, GPIO_AF1_TIM1 },    /* phiROG */
    { GPIOA, GPIO_PIN_1, GPIO_AF1_TIM2 },    /* ETR delay pulse (TIM2 CH3 on PA2 stays off, as in the original) */
    { GPIOC, GPIO_PIN_7, GPIO_AF2_TIM3 },    /* phiCLK */
    { GPIOB, GPIO_PIN_0, GPIO_AF2_TIM3 },
    { GPIOB, GPIO_PIN_1, GPIO_AF2_TIM3 },
    { GPIOC, GPIO_PIN_6, GPIO_AF3_TIM8 },    /* ADC sample strobes */
    { GPIOI, GPIO_PIN_6, GPIO_AF3_TIM8 },
    { GPIOA, GPIO_PIN_0, GPIO_AF3_TIM8 },    /* TIM8_ETR input */
};

static void pins_connect(bool on)
{
    GPIO_InitTypeDef g = {0};

    for (unsigned i = 0; i < sizeof(timer_pins) / sizeof(timer_pins[0]); i++) {
        g.Pin = timer_pins[i].pin;
        if (on) {
            g.Mode = GPIO_MODE_AF_PP;
            g.Pull = GPIO_NOPULL;
            g.Speed = GPIO_SPEED_FREQ_HIGH;
            g.Alternate = timer_pins[i].af;
        } else {
            g.Mode = GPIO_MODE_ANALOG;   /* high impedance while the sensor is off */
            g.Pull = GPIO_NOPULL;
        }
        HAL_GPIO_Init(timer_pins[i].port, &g);
    }

    /* ADC input */
    g.Pin = GPIO_PIN_2;
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &g);
}

/* ------------------------------------------------------------------ timers */

#define OCM_PWM1 (6U)
#define OCM_PWM2 (7U)

static void timers_config(void)
{
    const uint32_t pix = tim_clk / (1000000000U / PIXEL_NS);   /* timer ticks per pixel: 120 at 240 MHz */
    const uint32_t tick_2mhz = tim_clk / 2000000U;             /* 0.5 us ticks for TIM1 */

    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_TIM8_CLK_ENABLE();
    __HAL_RCC_TIM1_FORCE_RESET();  __HAL_RCC_TIM1_RELEASE_RESET();
    __HAL_RCC_TIM2_FORCE_RESET();  __HAL_RCC_TIM2_RELEASE_RESET();
    __HAL_RCC_TIM3_FORCE_RESET();  __HAL_RCC_TIM3_RELEASE_RESET();
    __HAL_RCC_TIM8_FORCE_RESET();  __HAL_RCC_TIM8_RELEASE_RESET();

    /* TIM1: line master */
    TIM1->PSC = tick_2mhz - 1;
    TIM1->ARR = line_us * 2 - 1;
    TIM1->CCR1 = ROG_US * 2;
    TIM1->CCMR1 = (OCM_PWM2 << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
    TIM1->CCER = TIM_CCER_CC1E | TIM_CCER_CC1P;                 /* active low: pin high for the first 5 us */
    TIM1->CR2 = (4U << TIM_CR2_MMS_Pos);                        /* TRGO = OC1REF */
    TIM1->SMCR = TIM_SMCR_MSM;
    TIM1->BDTR = TIM_BDTR_MOE;
    TIM1->DIER = TIM_DIER_UIE;
    TIM1->CR1 = TIM_CR1_ARPE;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->SR = 0;

    /* TIM2: one pulse per line, ~1 us ticks as in the original (divide by 121 at 120 MHz) */
    TIM2->PSC = (tim_clk / 120000000U) * 121 - 1;
    TIM2->ARR = 29;
    TIM2->CCR2 = 4;
    TIM2->CCR3 = 4;
    TIM2->CCMR1 = (OCM_PWM2 << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;
    TIM2->CCMR2 = (OCM_PWM2 << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;
    TIM2->CCER = TIM_CCER_CC2E;                                 /* original starts CH2 only */
    TIM2->SMCR = TIM_SMCR_MSM | (0U << TIM_SMCR_TS_Pos) | (6U << TIM_SMCR_SMS_Pos);  /* ITR0 = TIM1, trigger mode */
    TIM2->CR1 = TIM_CR1_OPM;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0;

    /* TIM3: pixel clock, gated by TIM1 */
    TIM3->PSC = 0;
    TIM3->ARR = pix - 1;
    TIM3->CCR2 = pix * 14 / 60;
    TIM3->CCR3 = pix * 14 / 60;
    TIM3->CCR4 = pix * 14 / 60;
    TIM3->CCMR1 = (OCM_PWM1 << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE;
    TIM3->CCMR2 = (OCM_PWM1 << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE |
                  (OCM_PWM1 << TIM_CCMR2_OC4M_Pos) | TIM_CCMR2_OC4PE;
    TIM3->CCER = TIM_CCER_CC2E | TIM_CCER_CC3E | TIM_CCER_CC4E;
    TIM3->CR2 = (6U << TIM_CR2_MMS_Pos);
    TIM3->SMCR = (0U << TIM_SMCR_TS_Pos) | (5U << TIM_SMCR_SMS_Pos);  /* ITR0 = TIM1, gated mode */
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR = 0;

    /* TIM8: 4096 ADC triggers after the falling edge on ETR (PA0) */
    TIM8->PSC = 0;
    TIM8->ARR = pix - 1;
    TIM8->RCR = SENSOR_SAMPLES - 1;
    TIM8->CCR1 = pix * 8 / 60;
    TIM8->CCR2 = pix * 8 / 60;
    TIM8->CCMR1 = (OCM_PWM1 << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE | TIM_CCMR1_OC1FE |
                  (OCM_PWM1 << TIM_CCMR1_OC2M_Pos) | TIM_CCMR1_OC2PE | TIM_CCMR1_OC2FE;
    TIM8->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
    TIM8->CR2 = (4U << TIM_CR2_MMS2_Pos) | (2U << TIM_CR2_MMS_Pos);   /* TRGO2 = OC1REF, TRGO = update */
    TIM8->AF1 = 0;                                                     /* ETR = pin */
    TIM8->SMCR = TIM_SMCR_ETP | TIM_SMCR_MSM | (7U << TIM_SMCR_TS_Pos) | (6U << TIM_SMCR_SMS_Pos);  /* ETRF, trigger */
    TIM8->BDTR = TIM_BDTR_MOE;
    TIM8->CR1 = TIM_CR1_OPM;
    TIM8->EGR = TIM_EGR_UG;
    TIM8->SR = 0;

    HAL_NVIC_SetPriority(TIM1_UP_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_IRQn);
}

/*
 * Two free-running outputs the original firmware starts at boot, before powering the
 * sensor (purpose on the carrier unknown; reproduced exactly):
 *   TIM13 CH1 on PA6: ~120 Hz, 99 % high (PSC 1000, ARR 1000, CCR 10, PWM2 at 120 MHz)
 *   TIM16 CH1 on PB8: 2 Hz, 50 %         (PSC 5999, ARR 9999, CCR 4999, PWM2 at 120 MHz)
 * Both timers run at 240 MHz here, so the prescalers are doubled.
 */
static void aux_outputs_start(void)
{
    GPIO_InitTypeDef g = {0};
    const uint32_t k = tim_clk / 120000000U;   /* 2 at 240 MHz */

    __HAL_RCC_TIM13_CLK_ENABLE();
    __HAL_RCC_TIM16_CLK_ENABLE();

    TIM13->CR1 = 0;
    TIM13->PSC = 1001 * k - 1;
    TIM13->ARR = 1000;
    TIM13->CCR1 = 10;
    TIM13->CCMR1 = (OCM_PWM2 << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE | TIM_CCMR1_OC1FE;
    TIM13->CCER = TIM_CCER_CC1E;
    TIM13->EGR = TIM_EGR_UG;
    TIM13->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;

    TIM16->CR1 = 0;
    TIM16->PSC = 6000 * k - 1;
    TIM16->ARR = 9999;
    TIM16->CCR1 = 4999;
    TIM16->CCMR1 = (OCM_PWM2 << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE | TIM_CCMR1_OC1FE;
    TIM16->CCER = TIM_CCER_CC1E;
    TIM16->BDTR = TIM_BDTR_MOE;
    TIM16->EGR = TIM_EGR_UG;
    TIM16->CR1 = TIM_CR1_CEN;

    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = GPIO_PIN_6;
    g.Alternate = GPIO_AF9_TIM13;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin = GPIO_PIN_8;
    g.Alternate = GPIO_AF1_TIM16;
    HAL_GPIO_Init(GPIOB, &g);
}

static void timers_stop(void)
{
    TIM1->CR1 &= ~TIM_CR1_CEN;
    TIM1->DIER = 0;
    HAL_NVIC_DisableIRQ(TIM1_UP_IRQn);
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM3->CR1 &= ~TIM_CR1_CEN;
    TIM8->CR1 &= ~TIM_CR1_CEN;
}

/* At each line start the previous line's 4096 conversions must be complete. */
void TIM1_UP_IRQHandler(void)
{
    if (TIM1->SR & TIM_SR_UIF) {
        TIM1->SR = ~TIM_SR_UIF;
        uint32_t remaining = ((DMA_Stream_TypeDef *)hdma.Instance)->NDTR;
        if (lines > 0 && (remaining % SENSOR_SAMPLES) != 0) {
            sync_errors++;
        }
    }
}

/* ------------------------------------------------------------------ ADC */

void HAL_ADC_MspInit(ADC_HandleTypeDef *h)
{
    __HAL_RCC_ADC3_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    hdma.Instance = DMA2_Stream0;
    hdma.Init.Request = DMA_REQUEST_ADC3;
    hdma.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma.Init.MemInc = DMA_MINC_ENABLE;
    hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma.Init.Mode = DMA_CIRCULAR;
    hdma.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    hdma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma);
    __HAL_LINKDMA(h, DMA_Handle, hdma);

    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}

void DMA2_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma);
}

static void line_done(const uint16_t *line)
{
    lines++;
    if (want_snap) {
        memcpy(snap, line, sizeof(snap));
        want_snap = false;
        snap_ready = true;
    }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *h) { (void)h; line_done(&adc_buf[0]); }
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *h)     { (void)h; line_done(&adc_buf[SENSOR_SAMPLES]); }
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *h)        { (void)h; dma_errors++; }

static bool adc_config(void)
{
    ADC_ChannelConfTypeDef ch = {0};

    /* The original firmware also writes VREFBUF (ENVR, 2.5 V), but the buffer never
     * becomes ready on this board (VREF+ is supplied externally), and images recorded by
     * the original firmware show the same levels as this code without it. Left off. */

    /* Keep PC2 and PC2_C separate, as the original firmware does (SYSCFG_PMCR.PC2SO) */
    HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC2, SYSCFG_SWITCH_PC2_OPEN);

    /* Synchronous ADC clock = HCLK / 2 = 120 MHz, the same 120 MHz the original
     * firmware used (it ran HCLK at 120 MHz with CKMODE = HCLK/1). */
    hadc.Instance = ADC3;
    hadc.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
    hadc.Init.Resolution = ADC_RESOLUTION_12B;
    hadc.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc.Init.LowPowerAutoWait = DISABLE;
    hadc.Init.ContinuousConvMode = DISABLE;
    hadc.Init.NbrOfConversion = 1;
    hadc.Init.DiscontinuousConvMode = DISABLE;
    hadc.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T8_TRGO2;
    hadc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_FALLING;
    hadc.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
    hadc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    hadc.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    hadc.Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(&hadc) != HAL_OK) {
        return false;
    }

    ch.Channel = ADC_CHANNEL_0;               /* ADC3_INP0 = PC2_C */
    ch.Rank = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
    ch.SingleDiff = ADC_SINGLE_ENDED;
    ch.OffsetNumber = ADC_OFFSET_NONE;
    ch.Offset = 0;
    if (HAL_ADC_ConfigChannel(&hadc, &ch) != HAL_OK) {
        return false;
    }
    return HAL_ADCEx_Calibration_Start(&hadc, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) == HAL_OK;
}

static void probe(const char *step)
{
    uint32_t t0 = HAL_GetTick(), sum = 0, bsum = 0;
    uint16_t lo = 0xffff, hi = 0;

    want_snap = true;
    while (!snap_ready) {
        if (HAL_GetTick() - t0 > 200) {
            log_printf("Self-test %-13s no line captured\r\n", step);
            return;
        }
    }
    for (uint32_t i = SENSOR_ACTIVE_FIRST; i <= SENSOR_ACTIVE_LAST; i++) {
        if (snap[i] < lo) lo = snap[i];
        if (snap[i] > hi) hi = snap[i];
        sum += snap[i];
    }
    for (uint32_t i = SENSOR_BLACK_FIRST; i <= SENSOR_BLACK_LAST; i++) bsum += snap[i];
    snap_ready = false;
    log_printf("Self-test %-13s active min %4u max %4u mean %4lu, black %4lu, first samples %u %u %u %u\r\n",
               step, lo, hi, sum / (SENSOR_ACTIVE_LAST - SENSOR_ACTIVE_FIRST + 1),
               bsum / (SENSOR_BLACK_LAST - SENSOR_BLACK_FIRST + 1), snap[0], snap[1], snap[2], snap[3]);
}

/* ------------------------------------------------------------------ public */

bool sensor_start(uint32_t lu)
{
    if (running) return true;
    if (lu < SENSOR_LINE_US_MIN) lu = SENSOR_LINE_US_MIN;
    if (lu > SENSOR_LINE_US_MAX) lu = SENSOR_LINE_US_MAX;
    line_us = lu;
    lines = sync_errors = dma_errors = 0;
    snap_ready = want_snap = false;

    /* APB2 (TIM1/TIM8) and APB1 (TIM2/TIM3) timer clocks are 2x PCLK when PCLK is divided */
    tim_clk = HAL_RCC_GetPCLK2Freq() * 2;

    if (!adc_config()) {
        last_error = "ADC setup";
        return false;
    }
    timers_config();

    /* Same order as the original: outputs running, then 9 V, then 5 V */
    aux_outputs_start();
    pins_connect(true);

    memset(adc_buf, 0, sizeof(adc_buf));
    if (HAL_ADC_Start_DMA(&hadc, (uint32_t *)adc_buf, ADC_BUF_LEN) != HAL_OK) {
        last_error = "ADC DMA start";
        pins_connect(false);
        return false;
    }

    /* Slaves first, then the master */
    TIM3->CR1 |= TIM_CR1_CEN;     /* gated: counts only while TIM1 TRGO is high */
    TIM1->CR1 |= TIM_CR1_CEN;
    running = true;

    /* Self-test: log the ADC level at each power step, to see whether it follows the sensor */
    probe("sensor off");
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, GPIO_PIN_SET);     /* 9 V on */
    HAL_Delay(500);
    probe("9 V on");
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_11, GPIO_PIN_RESET);   /* 5 V on */
    HAL_Delay(500);
    probe("9 V + 5 V on");
    return true;
}

void sensor_stop(void)
{
    if (!running) return;
    timers_stop();
    HAL_ADC_Stop_DMA(&hadc);
    pins_connect(false);          /* stop driving the sensor inputs before removing its power */
    power_off();
    running = false;
}

bool sensor_running(void)
{
    return running;
}

bool sensor_set_line_us(uint32_t lu)
{
    if (lu < SENSOR_LINE_US_MIN || lu > SENSOR_LINE_US_MAX) return false;
    line_us = lu;
    TIM1->ARR = lu * 2 - 1;       /* preloaded: applies from the next line */
    return true;
}

uint32_t sensor_line_us(void)
{
    return line_us;
}

bool sensor_snapshot(uint16_t dst[SENSOR_SAMPLES])
{
    if (!running) return false;
    if (!snap_ready) {
        want_snap = true;
        return false;
    }
    memcpy(dst, snap, sizeof(snap));
    snap_ready = false;
    want_snap = true;             /* ask for the next one */
    return true;
}

void sensor_get_stats(sensor_stats_t *s)
{
    s->lines = lines;
    s->sync_errors = sync_errors;
    s->dma_errors = dma_errors;
}
