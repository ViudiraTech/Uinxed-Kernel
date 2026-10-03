/*
 *
 *      rtl8139.h
 *      Realtek RTL8139 network controller driver header
 *
 *      2026/8/9 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_RTL8139_H_
#define INCLUDE_RTL8139_H_

#include <drivers/bus/pci.h>
#include <kernel/errno.h>

#define RTL8139_VENDOR_REALTEK 0x10ec // Realtek vendor ID
#define RTL8139_MTU            1500   // Default MTU

/* Register map (RTL8139D datasheet Rev 1.11). The classic RTL8139 exposes these registers through PCI I/O space. */
#define RTL8139_REG_IDR0    0x00 // MAC address, bytes 0-5
#define RTL8139_REG_MAR0    0x08 // multicast address filter
#define RTL8139_REG_TSD0    0x10 // transmit status, descriptor 0-3
#define RTL8139_REG_TSAD0   0x20 // transmit start address, descriptor 0-3
#define RTL8139_REG_RBSTART 0x30 // receive buffer start address
#define RTL8139_REG_CR      0x37 // command register (byte)
#define RTL8139_REG_CAPR    0x38 // current address of packet read (word)
#define RTL8139_REG_CBR     0x3a // current buffer address (word, read-only)
#define RTL8139_REG_IMR     0x3c // interrupt mask register (word)
#define RTL8139_REG_ISR     0x3e // interrupt status register (word, W1C)
#define RTL8139_REG_TCR     0x40 // transmit configuration register
#define RTL8139_REG_RCR     0x44 // receive configuration register
#define RTL8139_REG_9346CR  0x50 // 93C46 command register (byte)
#define RTL8139_REG_BMSR    0x64 // basic mode status register (word)

/* Command register (0x37) */
#define RTL8139_CR_BUFE  (1u << 0) // receive buffer empty
#define RTL8139_CR_TE    (1u << 2) // transmitter enable
#define RTL8139_CR_RE    (1u << 3) // receiver enable
#define RTL8139_CR_RESET (1u << 4) // software reset

/* Interrupt mask / status (0x3c/0x3e) */
#define RTL8139_ISR_ROK   (1u << 0) // receive OK
#define RTL8139_ISR_RER   (1u << 1) // receive error
#define RTL8139_ISR_TOK   (1u << 2) // transmit OK
#define RTL8139_ISR_TER   (1u << 3) // transmit error
#define RTL8139_ISR_RXOVW (1u << 4) // receive buffer overflow
#define RTL8139_ISR_PUN   (1u << 5) // packet underrun / link change
#define RTL8139_ISR_FOVW  (1u << 6) // receive FIFO overflow

/* Transmit configuration (0x40) */
#define RTL8139_TCR_IFG_NORMAL (3u << 24) // standard interframe gap
#define RTL8139_TCR_MXDMA      6u         // 1024-byte DMA bursts

/* Receive configuration (0x44) */
#define RTL8139_RCR_APM   (1u << 1) // accept physical match
#define RTL8139_RCR_AM    (1u << 2) // accept multicast
#define RTL8139_RCR_AB    (1u << 3) // accept broadcast
#define RTL8139_RCR_RXFTH 4u        // 256-byte RX FIFO threshold
#define RTL8139_RCR_MXDMA 6u        // 1024-byte DMA bursts

/* 93C46 command register (0x50) */
#define RTL8139_9346_UNLOCK 0xc0
#define RTL8139_9346_LOCK   0x00

/* Basic mode status register (0x64) */
#define RTL8139_BMSR_LINK (1u << 2) // valid link established

/* Transmit status descriptor (TSD) bits */
#define RTL8139_TX_LEN_MASK 0x1fff
#define RTL8139_TX_OWN      (1u << 13) // 1 = DMA complete, descriptor available
#define RTL8139_TX_TUN      (1u << 14) // transmit FIFO underrun
#define RTL8139_TX_TOK      (1u << 15) // transmit OK
#define RTL8139_TX_OWC      (1u << 29) // out of window collision
#define RTL8139_TX_TABT     (1u << 30) // transmit aborted
#define RTL8139_TX_ERTXTH   8u         // early transmit threshold: 8 * 32 = 256 bytes

/* Receive packet header status bits (written before each RX frame) */
#define RTL8139_RX_ROK  (1u << 0) // receive OK
#define RTL8139_RX_FAE  (1u << 1) // frame alignment error
#define RTL8139_RX_CRC  (1u << 2) // CRC error
#define RTL8139_RX_LONG (1u << 3) // frame exceeds 4K bytes
#define RTL8139_RX_RUNT (1u << 4) // runt packet
#define RTL8139_RX_ISE  (1u << 5) // invalid symbol error

typedef struct rtl8139_device rtl8139_device_t;

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
} rtl8139_stats_t;

#if CONFIG_RTL8139 && CONFIG_NET

/* Probe every explicitly supported Realtek controller in the PCI cache. */
int rtl8139_init(void);

/* Create per-device workers after scheduler initialization. */
int rtl8139_start_workers(void);

#else
static inline int rtl8139_init(void)
{
    return 0;
}
static inline int rtl8139_start_workers(void)
{
    return 0;
}
#endif

/* Probe one PCI function. The function must have a supported Realtek ID. */
int rtl8139_probe(pci_device_cache_t *pci);

/* Quiesce devices, release DMA memory, and unregister network adapters. */
void rtl8139_shutdown(void);

/* Synchronous transmit. Returns zero, -EAGAIN on backpressure, or an error. */
int rtl8139_transmit(rtl8139_device_t *device, const void *packet, size_t length);

/* Poll completed receive frames from task context. */
size_t rtl8139_poll(rtl8139_device_t *device, size_t budget);

/* True if the device link is up. */
int rtl8139_link_up(const rtl8139_device_t *device);

/* Return the device MAC address (6 bytes). */
const uint8_t *rtl8139_mac_address(const rtl8139_device_t *device);

/* Return a snapshot of device statistics. */
const rtl8139_stats_t *rtl8139_get_stats(const rtl8139_device_t *device);

/* Iterate registered RTL8139 devices in probe order. */
rtl8139_device_t *rtl8139_first_device(void);
rtl8139_device_t *rtl8139_next_device(rtl8139_device_t *device);

#endif // INCLUDE_RTL8139_H_
