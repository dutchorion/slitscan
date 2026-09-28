/*
 * RTC driver. The calendar keeps running on the coin cell while the camera is off,
 * so it is only initialised when it has never been set (RTC_ISR.INITS clear).
 * Entering init mode would stop the clock, so a running RTC is left untouched.
 */
#include <string.h>
#include "stm32h7xx_hal.h"
#include "rtclock.h"

static RTC_HandleTypeDef hrtc;

static int month_from_build_date(const char *m)
{
    static const char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (int i = 0; i < 12; i++) {
        if (strncmp(m, &names[i * 3], 3) == 0) {
            return i + 1;
        }
    }
    return 1;
}

static bool lse_start(void)
{
    RCC_OscInitTypeDef osc = {0};

    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY)) {
        return true;
    }
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState = RCC_LSE_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;
    return HAL_RCC_OscConfig(&osc) == HAL_OK;
}

rtclock_status_t rtclock_init(void)
{
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTC_CLK_ENABLE();

    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    hrtc.Init.AsynchPrediv = 127;
    hrtc.Init.SynchPrediv = 255;
    hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;

    const bool running = (RCC->BDCR & RCC_BDCR_RTCEN) &&
                         (RCC->BDCR & RCC_BDCR_RTCSEL) == RCC_RTCCLKSOURCE_LSE &&
                         (RCC->BDCR & RCC_BDCR_LSERDY) &&
                         (RTC->ISR & RTC_ISR_INITS);
    if (running) {
        hrtc.State = HAL_RTC_STATE_READY;
        hrtc.Lock = HAL_UNLOCKED;
        __HAL_RTC_WRITEPROTECTION_DISABLE(&hrtc);
        HAL_StatusTypeDef st = HAL_RTC_WaitForSynchro(&hrtc);
        __HAL_RTC_WRITEPROTECTION_ENABLE(&hrtc);
        return st == HAL_OK ? RTCLOCK_KEPT : RTCLOCK_ERROR;
    }

    if (!lse_start()) {
        return RTCLOCK_NO_LSE;
    }
    RCC_PeriphCLKInitTypeDef pclk = {0};
    pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        return RTCLOCK_ERROR;
    }
    __HAL_RCC_RTC_ENABLE();
    if (HAL_RTC_Init(&hrtc) != HAL_OK) {
        return RTCLOCK_ERROR;
    }

    /* __DATE__ = "Sep 28 2026", __TIME__ = "11:22:33" */
    const char *d = __DATE__, *tm = __TIME__;
    rtclock_time_t t = {
        .year = (uint16_t)((d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0')),
        .month = (uint8_t)month_from_build_date(d),
        .day = (uint8_t)((d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0')),
        .hour = (uint8_t)((tm[0] - '0') * 10 + (tm[1] - '0')),
        .min = (uint8_t)((tm[3] - '0') * 10 + (tm[4] - '0')),
        .sec = (uint8_t)((tm[6] - '0') * 10 + (tm[7] - '0')),
    };
    return rtclock_set(&t) ? RTCLOCK_SET_DEFAULT : RTCLOCK_ERROR;
}

bool rtclock_get(rtclock_time_t *t)
{
    RTC_TimeTypeDef tm;
    RTC_DateTypeDef dt;

    /* Time must be read before date to unlock the shadow registers */
    if (HAL_RTC_GetTime(&hrtc, &tm, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_GetDate(&hrtc, &dt, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }
    t->year = (uint16_t)(2000 + dt.Year);
    t->month = dt.Month;
    t->day = dt.Date;
    t->hour = tm.Hours;
    t->min = tm.Minutes;
    t->sec = tm.Seconds;
    return true;
}

bool rtclock_set(const rtclock_time_t *t)
{
    RTC_TimeTypeDef tm = {0};
    RTC_DateTypeDef dt = {0};

    tm.Hours = t->hour;
    tm.Minutes = t->min;
    tm.Seconds = t->sec;
    tm.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    tm.StoreOperation = RTC_STOREOPERATION_RESET;
    dt.Year = (uint8_t)(t->year - 2000);
    dt.Month = t->month;
    dt.Date = t->day;
    dt.WeekDay = RTC_WEEKDAY_MONDAY;   /* not used */
    return HAL_RTC_SetTime(&hrtc, &tm, RTC_FORMAT_BIN) == HAL_OK &&
           HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BIN) == HAL_OK;
}

const char *rtclock_status_str(rtclock_status_t s)
{
    switch (s) {
    case RTCLOCK_KEPT:        return "running, time kept";
    case RTCLOCK_SET_DEFAULT: return "was not set; started at firmware build time";
    case RTCLOCK_NO_LSE:      return "32.768 kHz crystal did not start";
    default:                  return "error";
    }
}
