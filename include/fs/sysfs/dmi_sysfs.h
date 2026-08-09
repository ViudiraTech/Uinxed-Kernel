/*
 *
 *      dmi_sysfs.h
 *      DMI / SMBIOS sysfs integration header
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DMI_SYSFS_H_
#define INCLUDE_DMI_SYSFS_H_

/*
 * Register /sys/class/dmi/id (system identity attributes) and
 * /sys/firmware/dmi/tables/DMI (raw SMBIOS structure table).
 */
void dmi_sysfs_init(void);

#endif // INCLUDE_DMI_SYSFS_H_
