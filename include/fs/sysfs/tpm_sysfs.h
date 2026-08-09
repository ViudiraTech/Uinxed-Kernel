/*
 *
 *      tpm_sysfs.h
 *      TPM sysfs class integration header
 *
 *      2026/8/6 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TPM_SYSFS_H_
#define INCLUDE_TPM_SYSFS_H_

/*
 * Register /sys/class/tpm/tpm0 and /sys/class/tpmrm/tpmrm0 with the
 * standard attribute files (version, firmware, caps, pcrs, timeouts).
 */
void tpm_sysfs_init(void);

#endif // INCLUDE_TPM_SYSFS_H_
