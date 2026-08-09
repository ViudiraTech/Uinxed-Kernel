/*
 *
 *      volpart.c
 *      FatFs logical-volume to partition mapping.
 *
 *      2026/5/22 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/fatfs/fatfs_disk.h>
#include <kernel/errno.h>
#include <kernel/printk.h>

PARTITION VolToPart[FF_VOLUMES];

int fatfs_assign_volume(uint8_t volume, uint8_t drive, uint8_t partition)
{
    if (volume >= FF_VOLUMES) {
        plogk("fat: assign volume %u out of range (max %u)\n", volume, FF_VOLUMES);
        return -EINVAL;
    }

    VolToPart[volume].pd = drive;
    VolToPart[volume].pt = partition;
    return EOK;
}

void fatfs_reset_volumes(void)
{
    for (uint8_t i = 0; i < FF_VOLUMES; i++) {
        VolToPart[i].pd = 0;
        VolToPart[i].pt = 0;
    }
}
