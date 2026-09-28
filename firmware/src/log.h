/* Text log: buffered in RAM, appended to LOG.TXT on the SD card by log_flush(). */
#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool log_flush(void);   /* needs a mounted card; keeps the text if writing fails */

#endif
