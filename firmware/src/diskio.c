/* FatFs low-level disk interface: drive 0 = microSD card */
#include "ff.h"
#include "diskio.h"
#include "sdcard.h"
#include "rtclock.h"

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return sdcard_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (!sdcard_ready() && !sdcard_init()) return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sdcard_ready()) return RES_NOTRDY;
    return sdcard_read(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sdcard_ready()) return RES_NOTRDY;
    return sdcard_write(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    sdcard_info_t info;

    if (pdrv != 0 || !sdcard_ready()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return sdcard_sync() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        sdcard_info(&info);
        *(LBA_t *)buff = info.blocks;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;     /* erase block size unknown */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* File timestamps from the RTC: bit31:25 year-1980, 24:21 month, 20:16 day, 15:11 h, 10:5 min, 4:0 s/2 */
DWORD get_fattime(void)
{
    rtclock_time_t t;
    if (!rtclock_get(&t)) {
        return ((DWORD)(2025 - 1980) << 25) | (1U << 21) | (1U << 16);
    }
    return ((DWORD)(t.year - 1980) << 25) | ((DWORD)t.month << 21) | ((DWORD)t.day << 16) |
           ((DWORD)t.hour << 11) | ((DWORD)t.min << 5) | ((DWORD)t.sec >> 1);
}
