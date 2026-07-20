/*-----------------------------------------------------------------------*/
/* Low level disk I/O module for FatFs over blockdev                     */
/*-----------------------------------------------------------------------*/

#include "ff.h"
#include "ffdiskio.h"
#include <blockdev.h>
#include <errno.h>
#include <fatfs_disk.h>
#include <printk.h>
#include <string.h>

static blockdev_device_t fatfs_devices[FF_VOLUMES];
static BYTE              fatfs_ready[FF_VOLUMES];
static BYTE              fatfs_bound[FF_VOLUMES];
static BYTE              fatfs_logged_sector0[FF_VOLUMES];

int fatfs_bind_device(uint8_t drive, const blockdev_device_t *device)
{
    if (drive >= FF_VOLUMES || !device) return -EINVAL;

    fatfs_devices[drive] = *device;
    fatfs_ready[drive]   = 1;
    fatfs_bound[drive]   = 1;
    return EOK;
}

void fatfs_unbind_device(uint8_t drive)
{
    if (drive >= FF_VOLUMES) return;
    fatfs_bound[drive] = 0;
    fatfs_ready[drive] = 0;
}

static DRESULT fatfs_open_drive(BYTE pdrv)
{
    if (pdrv >= FF_VOLUMES) return RES_PARERR;
    if (fatfs_bound[pdrv]) return RES_OK;
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

    if (blockdev_read_sectors(&fatfs_devices[pdrv], (uint32_t)sector, count, buff) != EOK) return RES_ERROR;

    if (!fatfs_logged_sector0[pdrv] && sector == 0 && count > 0) {
        fatfs_logged_sector0[pdrv] = 1;
        plogk("fatfs-disk: pdrv=%u sector0 sig=%02x%02x oem=%.8s\n", pdrv, buff[510], buff[511], (char *)(buff + 3));
    }

    return RES_OK;
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
