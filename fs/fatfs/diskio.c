/*-----------------------------------------------------------------------*/
/* Low level disk I/O module for FatFs over blockdev                     */
/*-----------------------------------------------------------------------*/

#include <blockdev.h>
#include <errno.h>
#include <string.h>

#include "ff.h"
#include "ffdiskio.h"

static blockdev_device_t fatfs_devices[FF_VOLUMES];
static BYTE              fatfs_ready[FF_VOLUMES];

static DRESULT fatfs_open_drive(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES) return RES_PARERR;
    if (blockdev_open_ide(pdrv, &fatfs_devices[pdrv]) != EOK) return RES_NOTRDY;

    fatfs_ready[pdrv] = 1;
    return RES_OK;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES) return STA_NOINIT;
    if (!fatfs_ready[pdrv] && fatfs_open_drive(pdrv) != RES_OK) return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return fatfs_open_drive(pdrv) == RES_OK ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (!buff || !count) return RES_PARERR;
    if (disk_status(pdrv) & STA_NOINIT) return RES_NOTRDY;

    return blockdev_read_sectors(&fatfs_devices[pdrv], (uint32_t)sector, count, buff) == EOK ? RES_OK : RES_ERROR;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (!buff || !count) return RES_PARERR;
    if (disk_status(pdrv) & STA_NOINIT) return RES_NOTRDY;

    return blockdev_write_sectors(&fatfs_devices[pdrv], (uint32_t)sector, count, buff) == EOK ? RES_OK : RES_ERROR;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (disk_status(pdrv) & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
        case CTRL_SYNC :
            return RES_OK;
        case GET_SECTOR_COUNT :
            if (!buff) return RES_PARERR;
            *(LBA_t *)buff = fatfs_devices[pdrv].sector_count;
            return RES_OK;
        case GET_SECTOR_SIZE :
            if (!buff) return RES_PARERR;
            *(WORD *)buff = (WORD)fatfs_devices[pdrv].sector_size;
            return RES_OK;
        case GET_BLOCK_SIZE :
            if (!buff) return RES_PARERR;
            *(DWORD *)buff = 1;
            return RES_OK;
        case CTRL_TRIM :
            return RES_OK;
        default :
            return RES_PARERR;
    }
}
