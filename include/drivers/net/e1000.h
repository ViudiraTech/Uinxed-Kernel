/*
 *
 *      e1000.h
 *      Intel e1000/e1000e network controller driver header
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_E1000_H_
#define INCLUDE_DRIVERS_E1000_H_

#include <drivers/bus/pci.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define E1000_VENDOR_INTEL 0x8086
#define E1000_MTU          1500

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

/* Probe every explicitly supported Intel controller in the PCI cache. */
int e1000_init(void);

/* Create per-device workers after scheduler initialization. */
int e1000_start_workers(void);

/* Probe one PCI function. The function must have a supported Intel ID. */
int e1000_probe(pci_device_cache_t *pci);

/* Quiesce devices, release DMA memory, and unregister network adapters. */
void e1000_shutdown(void);

/* Synchronous transmit. Returns zero, -EAGAIN on backpressure, or an error. */
int e1000_transmit(e1000_device_t *device, const void *packet, size_t length);

/* Poll completed receive descriptors from task context. */
size_t e1000_poll(e1000_device_t *device, size_t budget);

int                  e1000_link_up(const e1000_device_t *device);
const uint8_t       *e1000_mac_address(const e1000_device_t *device);
const e1000_stats_t *e1000_get_stats(const e1000_device_t *device);
e1000_device_t      *e1000_first_device(void);
e1000_device_t      *e1000_next_device(e1000_device_t *device);

/* Legacy IRQ hooks must provide shared dispatch; MSI is used otherwise. */
typedef void (*net_irq_handler_fn)(void *frame);

extern int  net_irq_claim_legacy(uint8_t irq, net_irq_handler_fn handler) __attribute__((weak));
extern void net_irq_release_legacy(uint8_t irq, net_irq_handler_fn handler) __attribute__((weak));

#endif
