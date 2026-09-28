/* Sony ILX514 line sensor: power, clock generation and ADC capture */
#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>
#include <stdbool.h>

#define SENSOR_SAMPLES     4096      /* ADC samples per line */
#define SENSOR_LINE_US_MIN 2500      /* readout of 4096 pixels at 500 ns takes ~2.1 ms */
#define SENSOR_LINE_US_MAX 32000

/* Where things land in a line of samples (first sample is sensor pixel D61) */
#define SENSOR_BLACK_FIRST 5         /* optical black: samples 5..30 */
#define SENSOR_BLACK_LAST  30
#define SENSOR_ACTIVE_FIRST 38       /* ~S1 */
#define SENSOR_ACTIVE_LAST  3955     /* ~S3918 */

typedef struct {
    uint32_t lines;          /* lines captured since start */
    uint32_t sync_errors;    /* line starts where the DMA was not on a line boundary */
    uint32_t dma_errors;
} sensor_stats_t;

bool sensor_start(uint32_t line_us);
void sensor_stop(void);
bool sensor_running(void);
bool sensor_set_line_us(uint32_t line_us);
uint32_t sensor_line_us(void);

/* Copy the most recent complete line (12-bit values). False if none since the last call. */
bool sensor_snapshot(uint16_t dst[SENSOR_SAMPLES]);
void sensor_get_stats(sensor_stats_t *s);
const char *sensor_last_error(void);   /* which step failed in sensor_start() */

#endif
