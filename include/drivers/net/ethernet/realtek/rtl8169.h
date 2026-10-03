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
#include <kernel/errno.h>

#define RTL8169_VENDOR_REALTEK 0x10ec
#define RTL8169_MTU            1500

/* Register map (RTL8169S/RTL8110S datasheet Rev 1.3). Access widths follow the datasheet; descriptor arrays must be 256-byte aligned. */
#define RTL8169_REG_IDR0      0x0000 // MAC address, bytes 0-5
#define RTL8169_REG_TNPDS     0x0020 // TX descriptor start address, 64-bit (low/high)
#define RTL8169_REG_CR        0x0037 // Command register (byte)
#define RTL8169_REG_TPPOLL    0x0038 // Transmit priority polling (byte)
#define RTL8169_REG_IMR       0x003c // Interrupt mask register (word)
#define RTL8169_REG_ISR       0x003e // Interrupt status register (word, W1C)
#define RTL8169_REG_TCR       0x0040 // Transmit configuration register
#define RTL8169_REG_RCR       0x0044 // Receive configuration register
#define RTL8169_REG_9346CR    0x0050 // 93C46/93C56 command register (byte)
#define RTL8169_REG_PHYSTATUS 0x006c // PHY(GMII/MII/TBI) status register (byte)
#define RTL8169_REG_RMS       0x00da // Receive packet maximum size (word)
#define RTL8169_REG_CPLUSCR   0x00e0 // C+ command register (word)
#define RTL8169_REG_RDSAR     0x00e4 // RX descriptor start address, 64-bit (low/high)
#define RTL8169_REG_MTPS      0x00ec // Max transmit packet size register (byte)

/* Command register (0x37) */
#define RTL8169_CR_TE    (1u << 2)
#define RTL8169_CR_RE    (1u << 3)
#define RTL8169_CR_RESET (1u << 4)

/* Transmit priority polling (0x38) */
#define RTL8169_TPPOLL_NPQ (1u << 6)

/* Interrupt mask / status (0x3c/0x3e) */
#define RTL8169_ISR_ROK     (1u << 0)
#define RTL8169_ISR_RER     (1u << 1)
#define RTL8169_ISR_TOK     (1u << 2)
#define RTL8169_ISR_TER     (1u << 3)
#define RTL8169_ISR_RDU     (1u << 4)
#define RTL8169_ISR_LINKCHG (1u << 5)
#define RTL8169_ISR_FOVW    (1u << 6)
#define RTL8169_ISR_TDU     (1u << 7)
#define RTL8169_ISR_SWINT   (1u << 8)
#define RTL8169_ISR_TIMEOUT (1u << 14)
#define RTL8169_ISR_SERR    (1u << 15)

/* Transmit configuration (0x40) */
#define RTL8169_TCR_IFG_NORMAL      (3u << 24)
#define RTL8169_TCR_MXDMA_UNLIMITED (7u << 8)

/* Receive configuration (0x44) */
#define RTL8169_RCR_AAP   (1u << 0)  // accept all packets (promiscuous)
#define RTL8169_RCR_APM   (1u << 1)  // accept physical match
#define RTL8169_RCR_AM    (1u << 2)  // accept multicast
#define RTL8169_RCR_AB    (1u << 3)  // accept broadcast
#define RTL8169_RCR_AR    (1u << 4)  // accept runt
#define RTL8169_RCR_AER   (1u << 5)  // accept error packets
#define RTL8169_RCR_MXDMA (7u << 8)  // unlimited DMA burst
#define RTL8169_RCR_RXFTH (7u << 13) // no FIFO threshold

/* C+ command register (0xe0) */
#define RTL8169_CPLUS_DAC (1u << 4) // PCI dual address cycle (64-bit DMA)

/* 93C46/93C56 command register (0x50) */
#define RTL8169_9346_UNLOCK 0xc0
#define RTL8169_9346_LOCK   0x00

/* PHY status register (0x6c) */
#define RTL8169_PHYSTATUS_LINKSTS (1u << 1)

/* Descriptor dword0 bits */
#define RTL8169_DESC_OWN    (1u << 31)
#define RTL8169_DESC_EOR    (1u << 30)
#define RTL8169_DESC_FS     (1u << 29)
#define RTL8169_DESC_LS     (1u << 28)
#define RTL8169_TX_LEN_MASK 0x0000ffff
#define RTL8169_RX_LEN_MASK 0x00003fff

/* RX status error summary: RWT(22) | RES(21) | RUNT(20) | CRC(19) */
#define RTL8169_RX_ERROR_MASK (0x0fu << 19)

/* Descriptor dword0 carries ownership/status/length; dword2/3 hold the buffer address. */
typedef struct {
        uint32_t command;  // dword0: ownership/status/length
        uint32_t vlan;     // dword1: VLAN tag (unused)
        uint32_t low_buf;  // dword2: low 32 bits of buffer address
        uint32_t high_buf; // dword3: high 32 bits of buffer address
} __attribute__((packed)) rtl8169_desc_t;

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

#if CONFIG_RTL8169 && CONFIG_NET

/* Probe every explicitly supported Realtek controller in the PCI cache. */
int rtl8169_init(void);

/* Create per-device workers after scheduler initialization. */
int rtl8169_start_workers(void);

#else
static inline int rtl8169_init(void)
{
    return 0;
}
static inline int rtl8169_start_workers(void)
{
    return 0;
}
#endif

/* Probe one PCI function. The function must have a supported Realtek ID. */
int rtl8169_probe(pci_device_cache_t *pci);

/* Quiesce devices, release DMA memory, and unregister network adapters. */
void rtl8169_shutdown(void);

/* Synchronous transmit. Returns zero, -EAGAIN on backpressure, or an error. */
int rtl8169_transmit(rtl8169_device_t *device, const void *packet, size_t length);

/* Poll completed receive descriptors from task context. */
size_t rtl8169_poll(rtl8169_device_t *device, size_t budget);

/* True if the device link is up. */
int rtl8169_link_up(const rtl8169_device_t *device);

/* Return the device MAC address (6 bytes). */
const uint8_t *rtl8169_mac_address(const rtl8169_device_t *device);

/* Return a snapshot of device statistics. */
const rtl8169_stats_t *rtl8169_get_stats(const rtl8169_device_t *device);

/* Iterate registered RTL8169 devices in probe order. */
rtl8169_device_t *rtl8169_first_device(void);
rtl8169_device_t *rtl8169_next_device(rtl8169_device_t *device);

#endif // INCLUDE_RTL8169_H_
