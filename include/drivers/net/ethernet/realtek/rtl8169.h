/*
 *
 *      rtl8169.h
 *      Realtek RTL8169 network controller driver header
 *
 *      2026/8/9 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RTL8169_H_
#define INCLUDE_RTL8169_H_

#include <drivers/bus/pci.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define RTL8169_VENDOR_REALTEK 0x10ec
#define RTL8169_MTU            1500

typedef struct rtl8169_device rtl8169_device_t;

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
} rtl8169_stats_t;

/* Probe every explicitly supported Realtek controller in the PCI cache. */
int rtl8169_init(void);

/* Create per-device workers after scheduler initialization. */
int rtl8169_start_workers(void);

/* Probe one PCI function. The function must have a supported Realtek ID. */
int rtl8169_probe(pci_device_cache_t *pci);

/* Quiesce devices, release DMA memory, and unregister network adapters. */
void rtl8169_shutdown(void);

/* Synchronous transmit. Returns zero, -EAGAIN on backpressure, or an error. */
int rtl8169_transmit(rtl8169_device_t *device, const void *packet, size_t length);

/* Poll completed receive descriptors from task context. */
size_t rtl8169_poll(rtl8169_device_t *device, size_t budget);

int                    rtl8169_link_up(const rtl8169_device_t *device);
const uint8_t         *rtl8169_mac_address(const rtl8169_device_t *device);
const rtl8169_stats_t *rtl8169_get_stats(const rtl8169_device_t *device);
rtl8169_device_t      *rtl8169_first_device(void);
rtl8169_device_t      *rtl8169_next_device(rtl8169_device_t *device);

/* Legacy IRQ hooks must provide shared dispatch; MSI is used otherwise. */
#ifndef NET_IRQ_HANDLER_FN_TYPEDEF
#    define NET_IRQ_HANDLER_FN_TYPEDEF
typedef void (*net_irq_handler_fn)(void *frame);
#endif

extern int  net_irq_claim_legacy(uint8_t irq, net_irq_handler_fn handler) __attribute__((weak));
extern void net_irq_release_legacy(uint8_t irq, net_irq_handler_fn handler) __attribute__((weak));

#endif // INCLUDE_RTL8169_H_
