/*-----------------------------------------------------------------------*/
/* FatFs logical-volume to partition mapping                             */
/*-----------------------------------------------------------------------*/

#include <errno.h>
#include <fatfs_disk.h>
#include <printk.h>

PARTITION VolToPart[FF_VOLUMES];

int fatfs_assign_volume(uint8_t volume, uint8_t drive, uint8_t partition)
{
    if (volume >= FF_VOLUMES) return -EINVAL;

    VolToPart[volume].pd = drive;
    VolToPart[volume].pt = partition;
    plogk("fatfs: volume %u -> pd %u, pt %u\n", volume, drive, partition);
    return EOK;
}

void fatfs_reset_volumes(void)
{
    for (uint8_t i = 0; i < FF_VOLUMES; i++) {
        VolToPart[i].pd = 0;
        VolToPart[i].pt = 0;
    }
}
