#include <stdarg.h>
#include <stdio.h>
#include "ff.h"
#include "log.h"

#define LOG_FILE "LOG.TXT"

static char buf[8192];
static unsigned len;
static FIL file;         /* static: FatFs buffers must not live on the DTCM stack */

void log_printf(const char *fmt, ...)
{
    va_list ap;

    if (len >= sizeof(buf) - 1) {
        return;
    }
    va_start(ap, fmt);
    int n = vsnprintf(&buf[len], sizeof(buf) - len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        len += (unsigned)n;
        if (len > sizeof(buf) - 1) {
            len = sizeof(buf) - 1;
        }
    }
}

bool log_flush(void)
{
    UINT written;

    if (len == 0) {
        return true;
    }
    if (f_open(&file, LOG_FILE, FA_OPEN_APPEND | FA_WRITE) != FR_OK) {
        return false;
    }
    FRESULT fr = f_write(&file, buf, len, &written);
    FRESULT fc = f_close(&file);
    if (fr != FR_OK || fc != FR_OK || written != len) {
        return false;
    }
    len = 0;
    return true;
}
