/* Battery-backed real-time clock (LSE 32.768 kHz, CR2032 on VBAT) */
#ifndef RTCLOCK_H
#define RTCLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t year;   /* 2000..2099 */
    uint8_t month, day, hour, min, sec;
} rtclock_time_t;

typedef enum {
    RTCLOCK_KEPT,        /* clock was already running: time kept */
    RTCLOCK_SET_DEFAULT, /* clock was not set: started at the firmware build time */
    RTCLOCK_NO_LSE,      /* 32.768 kHz crystal did not start */
    RTCLOCK_ERROR,
} rtclock_status_t;

rtclock_status_t rtclock_init(void);
bool rtclock_get(rtclock_time_t *t);
bool rtclock_set(const rtclock_time_t *t);
const char *rtclock_status_str(rtclock_status_t s);

#endif
