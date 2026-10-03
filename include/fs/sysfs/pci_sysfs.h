/*
 *
 *      pci_sysfs.h
 *      PCI bus and device sysfs integration header
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PCI_SYSFS_H_
#define INCLUDE_PCI_SYSFS_H_

/* Register /sys/bus/pci/ and /sys/devices/pci* topology. */
#if CONFIG_SYSFS
void pci_sysfs_init(void);
#else
static inline void pci_sysfs_init(void) {}
#endif

#endif // INCLUDE_PCI_SYSFS_H_
