/*
 *
 *      diskio.c
 *      Low level disk I/O module for FatFs over blockdev.
 *
 *      2026/5/22 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/core/blockdev.h>
#include <fs/fatfs/fatfs_disk.h>
#include <fs/fatfs/ff.h>
#include <fs/fatfs/ffdiskio.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

static blockdev_device_t fatfs_devices[FF_VOLUMES];
static BYTE              fatfs_ready[FF_VOLUMES];
static BYTE              fatfs_bound[FF_VOLUMES];

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
    if (blockdev_open_drive(pdrv, &fatfs_devices[pdrv]) != EOK) {
        plogk("fatfs: Drive %u: block device open failed.\n", pdrv);
        return RES_NOTRDY;
    }

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

    if (blockdev_read_sectors(&fatfs_devices[pdrv], sector, count, buff) != EOK) {
        plogk("fatfs: Drive %u: sector read failed at %llu (count %u)\n", pdrv, (unsigned long long)sector, count);
        return RES_ERROR;
    }

    return RES_OK;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (!buff || !count) return RES_PARERR;
    if (disk_status(pdrv) & STA_NOINIT) return RES_NOTRDY;

    if (blockdev_write_sectors(&fatfs_devices[pdrv], sector, count, buff) != EOK) {
        plogk("fatfs: Drive %u: sector write failed at %llu (count %u)\n", pdrv, (unsigned long long)sector, count);
        return RES_ERROR;
    }

    return RES_OK;
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
