/*
 *
 *      e1000.h
 *      Intel e1000/e1000e network controller driver header
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_E1000_H_
#define INCLUDE_E1000_H_

#include <drivers/bus/pci.h>
#include <kernel/errno.h>

#define E1000_VENDOR_INTEL 0x8086
#define E1000_MTU          1500

#define E1000_REG_CTRL     0x0000
#define E1000_REG_STATUS   0x0008
#define E1000_REG_EECD     0x0010
#define E1000_REG_EERD     0x0014
#define E1000_REG_CTRL_EXT 0x0018
#define E1000_REG_ICR      0x00c0
#define E1000_REG_ITR      0x00c4
#define E1000_REG_ICS      0x00c8
#define E1000_REG_IMS      0x00d0
#define E1000_REG_IMC      0x00d8
#define E1000_REG_RCTL     0x0100
#define E1000_REG_TCTL     0x0400
#define E1000_REG_TIPG     0x0410
#define E1000_REG_RDBAL    0x2800
#define E1000_REG_RDBAH    0x2804
#define E1000_REG_RDLEN    0x2808
#define E1000_REG_RDH      0x2810
#define E1000_REG_RDT      0x2818
#define E1000_REG_RDTR     0x2820
#define E1000_REG_RADV     0x282c
#define E1000_REG_TDBAL    0x3800
#define E1000_REG_TDBAH    0x3804
#define E1000_REG_TDLEN    0x3808
#define E1000_REG_TDH      0x3810
#define E1000_REG_TDT      0x3818
#define E1000_REG_TIDV     0x3820
#define E1000_REG_TADV     0x382c
#define E1000_REG_RAL0     0x5400
#define E1000_REG_RAH0     0x5404
#define E1000_REG_MTA      0x5200

#define E1000_CTRL_SLU          (1u << 6)
#define E1000_CTRL_RST          (1u << 26)
#define E1000_CTRL_EXT_DRV_LOAD (1u << 28)
#define E1000_STATUS_LU         (1u << 1)
#define E1000_RAH_AV            (1u << 31)

#define E1000_RCTL_EN         (1u << 1)
#define E1000_RCTL_BAM        (1u << 15)
#define E1000_RCTL_SECRC      (1u << 26)
#define E1000_TCTL_EN         (1u << 1)
#define E1000_TCTL_PSP        (1u << 3)
#define E1000_TCTL_CT_SHIFT   4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_ICR_TXDW   (1u << 0)
#define E1000_ICR_LSC    (1u << 2)
#define E1000_ICR_RXSEQ  (1u << 3)
#define E1000_ICR_RXDMT0 (1u << 4)
#define E1000_ICR_RXO    (1u << 6)
#define E1000_ICR_RXT0   (1u << 7)

#define E1000_RXD_STAT_DD  (1u << 0)
#define E1000_RXD_STAT_EOP (1u << 1)
#define E1000_TXD_STAT_DD  (1u << 0)
#define E1000_TXD_STAT_EC  (1u << 1)
#define E1000_TXD_STAT_LC  (1u << 2)
#define E1000_TXD_STAT_TU  (1u << 3)

#define E1000_TXD_CMD_EOP  (1u << 0)
#define E1000_TXD_CMD_IFCS (1u << 1)
#define E1000_TXD_CMD_RS   (1u << 3)

typedef struct e1000_device e1000_device_t;

typedef struct {
        uint64_t rx_packets;
        uint64_t rx_bytes;
        uint64_t rx_dropped;
        uint64_t rx_errors;
        uint64_t rx_overruns;
        uint64_t tx_packets;
        uint64_t tx_bytes;
        uint64_t tx_dropped;
        uint64_t tx_errors;
        uint64_t tx_busy;
        uint64_t interrupts;
        uint64_t link_changes;
} e1000_stats_t;

#if CONFIG_E1000 && CONFIG_NET

/* Probe every explicitly supported Intel controller in the PCI cache. */
int e1000_init(void);

/* Create per-device workers after scheduler initialization. */
int e1000_start_workers(void);

#else
static inline int e1000_init(void)
{
    return 0;
}
static inline int e1000_start_workers(void)
{
    return 0;
}
#endif

/* Probe one PCI function. The function must have a supported Intel ID. */
int e1000_probe(pci_device_cache_t *pci);

/* Quiesce devices, release DMA memory, and unregister network adapters. */
void e1000_shutdown(void);

/* Synchronous transmit. Returns zero, -EAGAIN on backpressure, or an error. */
int e1000_transmit(e1000_device_t *device, const void *packet, size_t length);

/* Poll completed receive descriptors from task context. */
size_t e1000_poll(e1000_device_t *device, size_t budget);

/* True if the device link is up. */
int e1000_link_up(const e1000_device_t *device);

/* Return the device MAC address (6 bytes). */
const uint8_t *e1000_mac_address(const e1000_device_t *device);

/* Return a snapshot of device statistics. */
const e1000_stats_t *e1000_get_stats(const e1000_device_t *device);

/* Iterate registered e1000 devices in probe order. */
e1000_device_t *e1000_first_device(void);
e1000_device_t *e1000_next_device(e1000_device_t *device);

#endif // INCLUDE_E1000_H_
