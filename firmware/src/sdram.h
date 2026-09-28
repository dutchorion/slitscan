/* 32 MB W9825G6KH SDRAM on FMC bank 1 */
#ifndef SDRAM_H
#define SDRAM_H

#include <stdint.h>
#include <stdbool.h>

#define SDRAM_BASE 0xC0000000UL
#define SDRAM_SIZE (32UL * 1024 * 1024)

typedef struct {
    bool     ok;
    uint32_t fail_addr;      /* first failing address, 0 if none */
    uint32_t fail_expected;
    uint32_t fail_read;
    const char *fail_stage;
    uint32_t write_kbps;     /* sequential 32-bit write speed, KB/s */
    uint32_t read_kbps;
} sdram_test_t;

bool sdram_init(void);
void sdram_test(sdram_test_t *res);

#endif
