/*
 *
 *      rtl8169.c
 *      Realtek RTL8169 network controller driver
 *
 *      2026/8/9 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/idt.h>
#include <chipset/common.h>
#include <drivers/misc/apic.h>
#include <drivers/net/rtl8169.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <net/netdev.h>
#include <net/pbuf.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <sync/spin_lock.h>

#define RTL8169_MAX_DEVICES       8
#define RTL8169_RX_COUNT          256
#define RTL8169_TX_COUNT          256
#define RTL8169_BUFFER_SIZE       2048 /* multiple of 8, one RX buffer per descriptor */
#define RTL8169_MAX_FRAME_SIZE    (RTL8169_MTU + 18)
#define RTL8169_CRC_LEN           4    /* RX frame length reported by the chip includes CRC */
#define RTL8169_WORK_BUDGET       64
#define RTL8169_TX_RECLAIM_BUDGET 64
#define RTL8169_RESET_TIMEOUT_US  100000

/*
 * Register map (RTL8169S/RTL8110S datasheet Rev 1.3).
 * Access widths follow the datasheet; descriptor arrays must be
 * 256-byte aligned.
 */
#define RTL8169_REG_IDR0      0x0000 /* MAC address, bytes 0-5 */
#define RTL8169_REG_TNPDS     0x0020 /* TX descriptor start address, 64-bit (low/high) */
#define RTL8169_REG_CR        0x0037 /* Command register (byte) */
#define RTL8169_REG_TPPOLL    0x0038 /* Transmit priority polling (byte) */
#define RTL8169_REG_IMR       0x003c /* Interrupt mask register (word) */
#define RTL8169_REG_ISR       0x003e /* Interrupt status register (word, W1C) */
#define RTL8169_REG_TCR       0x0040 /* Transmit configuration register */
#define RTL8169_REG_RCR       0x0044 /* Receive configuration register */
#define RTL8169_REG_9346CR    0x0050 /* 93C46/93C56 command register (byte) */
#define RTL8169_REG_PHYSTATUS 0x006c /* PHY(GMII/MII/TBI) status register (byte) */
#define RTL8169_REG_RMS       0x00da /* Receive packet maximum size (word) */
#define RTL8169_REG_CPLUSCR   0x00e0 /* C+ command register (word) */
#define RTL8169_REG_RDSAR     0x00e4 /* RX descriptor start address, 64-bit (low/high) */
#define RTL8169_REG_MTPS      0x00ec /* Max transmit packet size register (byte) */

/* Command register (0x37) */
#define RTL8169_CR_TE      (1u << 2)
#define RTL8169_CR_RE      (1u << 3)
#define RTL8169_CR_RESET   (1u << 4)

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

#define RTL8169_INT_MASK    (RTL8169_ISR_ROK | RTL8169_ISR_RER | RTL8169_ISR_TOK | RTL8169_ISR_TER | \
                             RTL8169_ISR_RDU | RTL8169_ISR_LINKCHG | RTL8169_ISR_FOVW | RTL8169_ISR_TDU)
#define RTL8169_RX_INT_MASK (RTL8169_ISR_ROK | RTL8169_ISR_RER | RTL8169_ISR_RDU | RTL8169_ISR_FOVW)
#define RTL8169_WORK_INITIAL (RTL8169_ISR_ROK | RTL8169_ISR_TOK | RTL8169_ISR_LINKCHG)

/* Transmit configuration (0x40) */
#define RTL8169_TCR_IFG_NORMAL      (3u << 24)
#define RTL8169_TCR_MXDMA_UNLIMITED (7u << 8)

/* Receive configuration (0x44) */
#define RTL8169_RCR_AAP   (1u << 0) /* accept all packets (promiscuous) */
#define RTL8169_RCR_APM   (1u << 1) /* accept physical match */
#define RTL8169_RCR_AM    (1u << 2) /* accept multicast */
#define RTL8169_RCR_AB    (1u << 3) /* accept broadcast */
#define RTL8169_RCR_AR    (1u << 4) /* accept runt */
#define RTL8169_RCR_AER   (1u << 5) /* accept error packets */
#define RTL8169_RCR_MXDMA (7u << 8) /* unlimited DMA burst */
#define RTL8169_RCR_RXFTH (7u << 13) /* no FIFO threshold */

/* C+ command register (0xe0) */
#define RTL8169_CPLUS_DAC (1u << 4) /* PCI dual address cycle (64-bit DMA) */

/* 93C46/93C56 command register (0x50) */
#define RTL8169_9346_UNLOCK 0xc0
#define RTL8169_9346_LOCK   0x00

/* PHY status register (0x6c) */
#define RTL8169_PHYSTATUS_LINKSTS (1u << 1)

/* Descriptor dword0 bits */
#define RTL8169_DESC_OWN (1u << 31)
#define RTL8169_DESC_EOR (1u << 30)
#define RTL8169_DESC_FS  (1u << 29)
#define RTL8169_DESC_LS  (1u << 28)
#define RTL8169_TX_LEN_MASK 0x0000ffff
#define RTL8169_RX_LEN_MASK 0x00003fff

/* RX status error summary: RWT(22) | RES(21) | RUNT(20) | CRC(19) */
#define RTL8169_RX_ERROR_MASK (0x0fu << 19)

/*
 * Device IDs that use the classic RTL8169 descriptor and register
 * layout. The RTL8168 (10ec:8168) uses a different TX descriptor and
 * register map and is intentionally not listed here.
 */
typedef struct {
        uint16_t vendor;
        uint16_t device;
} rtl8169_id_t;

static const rtl8169_id_t rtl8169_ids[] = {
    {0x10ec, 0x8161}, /* RTL8169/RTL8111SC */
    {0x10ec, 0x8169}, /* RTL8169 */
    {0x1259, 0xc107}, /* Kontron */
    {0x1737, 0x1032}, /* Linksys EG1032 */
    {0x16ec, 0x0116}, /* US Robotics */
};

typedef struct {
        uint32_t command;  /* dword0: ownership/status/length */
        uint32_t vlan;     /* dword1: VLAN tag (unused) */
        uint32_t low_buf;  /* dword2: low 32 bits of buffer address */
        uint32_t high_buf; /* dword3: high 32 bits of buffer address */
} __attribute__((packed)) rtl8169_desc_t;

typedef struct rtl8169_device {
        pci_device_cache_t       *pci;
        volatile uint8_t         *mmio;
        uint64_t                  mmio_phys;
        uint32_t                  mmio_size;
        uint16_t                  device_id;
        uint16_t                  features;
        uint16_t                  saved_command;
        uint8_t                   mac[6];
        uint8_t                   irq;
        int                       vector;
        int                       using_msi;
        int                       using_legacy;
        int                       using_direct_legacy;
        int                       running;
        int                       stopping;
        int                       link_up;
        uint8_t                   irq_slot;
        volatile uint32_t         irq_active;
        uint32_t                  work_pending;
        uint32_t                  interrupts_pending;
        int                       worker_started;
        int                       worker_exited;
        task_t                   *worker_task;
        wait_queue_t              work_wait;
        wait_queue_t              exit_wait;
        spinlock_t                work_lock;
        volatile rtl8169_desc_t  *rx_ring;
        volatile rtl8169_desc_t  *tx_ring;
        uint64_t                  rx_ring_phys;
        uint64_t                  tx_ring_phys;
        uint64_t                  rx_buffer_phys[RTL8169_RX_COUNT];
        uint64_t                  tx_buffer_phys[RTL8169_TX_COUNT];
        uint16_t                  rx_next;
        uint16_t                  tx_next;
        uint16_t                  tx_clean;
        uint16_t                  tx_used;
        spinlock_t                rx_lock;
        spinlock_t                tx_lock;
        rtl8169_stats_t           stats;
        net_device_t              netdev;
        int                       netdev_registered;
        struct rtl8169_device    *next;
} rtl8169_device_t;

static rtl8169_device_t *rtl8169_devices;
static size_t            rtl8169_device_count;
static rtl8169_device_t *rtl8169_irq_slots[RTL8169_MAX_DEVICES];
static spinlock_t        rtl8169_irq_lock;
static int               rtl8169_scheduler_ready;

static inline uint8_t rtl8169_read8(const rtl8169_device_t *device, uint32_t reg)
{
    return *(volatile uint8_t *)(device->mmio + reg);
}

static inline uint16_t rtl8169_read16(const rtl8169_device_t *device, uint32_t reg)
{
    return *(volatile uint16_t *)(device->mmio + reg);
}

static inline uint32_t rtl8169_read32(const rtl8169_device_t *device, uint32_t reg)
{
    return *(volatile uint32_t *)(device->mmio + reg);
}

static inline void rtl8169_write8(rtl8169_device_t *device, uint32_t reg, uint8_t value)
{
    *(volatile uint8_t *)(device->mmio + reg) = value;
}

static inline void rtl8169_write16(rtl8169_device_t *device, uint32_t reg, uint16_t value)
{
    *(volatile uint16_t *)(device->mmio + reg) = value;
}

static inline void rtl8169_write32(rtl8169_device_t *device, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(device->mmio + reg) = value;
}

static inline void rtl8169_write_flush(rtl8169_device_t *device)
{
    (void)rtl8169_read32(device, RTL8169_REG_PHYSTATUS);
}

static const rtl8169_id_t *rtl8169_match(uint16_t vendor, uint16_t device)
{
    for (size_t i = 0; i < sizeof(rtl8169_ids) / sizeof(rtl8169_ids[0]); i++)
        if (rtl8169_ids[i].vendor == vendor && rtl8169_ids[i].device == device)
            return &rtl8169_ids[i];
    return NULL;
}

static int rtl8169_valid_mac(const uint8_t mac[6])
{
    uint8_t any = 0;
    uint8_t all = 0xff;
    for (size_t i = 0; i < 6; i++) {
        any |= mac[i];
        all &= mac[i];
    }
    return any != 0 && all != 0xff && !(mac[0] & 1);
}

static int rtl8169_map_bar(rtl8169_device_t *device)
{
    for (uint32_t bar = 0; bar < 6; bar++) {
        base_address_register_t info = get_base_address_register(device->pci, bar);
        uint32_t                raw;
        uint64_t                phys;

        if (info.type != mem_mapping || !info.size) continue;
        raw = read_bar_n(device->pci, bar);
        if (raw == 0xffffffff || (raw & 1) || (((raw >> 1) & 3) == BAR_Reserved)) continue;
        phys = raw & ~0xfull;
        if (((raw >> 1) & 3) == BAR_S64) {
            uint32_t high = read_bar_n(device->pci, bar + 1);
            if (high == 0xffffffff) continue;
            phys |= (uint64_t)high << 32;
        }
        if (!phys) continue;

        device->mmio_size = info.size & ~BAR_64BIT_FLAG;
        if (device->mmio_size < RTL8169_REG_RDSAR + 8) continue;
        if (phys + device->mmio_size < phys) continue;
        uint64_t start = phys & ~(PAGE_4K_SIZE - 1);
        uint64_t end   = (phys + device->mmio_size + PAGE_4K_SIZE - 1) & ~(PAGE_4K_SIZE - 1);
        page_map_range_to(get_kernel_pagedir(), start, end - start, PTE_MMIO_FLAGS);
        device->mmio_phys = phys;
        device->mmio      = (volatile uint8_t *)phys_to_virt(phys);
        return 0;
    }
    return -ENODEV;
}

static int rtl8169_reset(rtl8169_device_t *device)
{
    rtl8169_write8(device, RTL8169_REG_CR, RTL8169_CR_RESET);
    rtl8169_write_flush(device);
    for (uint32_t i = 0; i < RTL8169_RESET_TIMEOUT_US; i++) {
        if (!(rtl8169_read8(device, RTL8169_REG_CR) & RTL8169_CR_RESET)) return 0;
        usleep(1);
    }
    return -ETIMEDOUT;
}

static void rtl8169_read_mac(rtl8169_device_t *device)
{
    for (size_t i = 0; i < 6; i++) device->mac[i] = rtl8169_read8(device, RTL8169_REG_IDR0 + (uint32_t)i);
    if (!rtl8169_valid_mac(device->mac)) {
        plogk("rtl8169: %04x:%04x: invalid MAC address, using fallback.\n",
              (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
        for (size_t i = 0; i < 6; i++) device->mac[i] = i;
        device->mac[0] &= ~1u; /* ensure a unicast, locally administered address */
        device->mac[0] |= 2u;
    }
}

static void rtl8169_free_dma(rtl8169_device_t *device)
{
    for (size_t i = 0; i < RTL8169_RX_COUNT; i++) {
        if (device->rx_buffer_phys[i]) free_frames(device->rx_buffer_phys[i], 1);
        device->rx_buffer_phys[i] = 0;
    }
    for (size_t i = 0; i < RTL8169_TX_COUNT; i++) {
        if (device->tx_buffer_phys[i]) free_frames(device->tx_buffer_phys[i], 1);
        device->tx_buffer_phys[i] = 0;
    }
    if (device->rx_ring_phys) free_frames(device->rx_ring_phys, 1);
    if (device->tx_ring_phys) free_frames(device->tx_ring_phys, 1);
    device->rx_ring_phys = device->tx_ring_phys = 0;
    device->rx_ring                             = NULL;
    device->tx_ring                             = NULL;
}

static int rtl8169_alloc_dma(rtl8169_device_t *device)
{
    device->rx_ring_phys = alloc_frames(1);
    if (!device->rx_ring_phys) return -ENOMEM;
    device->tx_ring_phys = alloc_frames(1);
    if (!device->tx_ring_phys) return -ENOMEM;
    device->rx_ring = (volatile rtl8169_desc_t *)phys_to_virt(device->rx_ring_phys);
    device->tx_ring = (volatile rtl8169_desc_t *)phys_to_virt(device->tx_ring_phys);
    memset((void *)device->rx_ring, 0, PAGE_4K_SIZE);
    memset((void *)device->tx_ring, 0, PAGE_4K_SIZE);

    for (size_t i = 0; i < RTL8169_RX_COUNT; i++) {
        device->rx_buffer_phys[i] = alloc_frames(1);
        if (!device->rx_buffer_phys[i]) return -ENOMEM;
        uint32_t cmd = RTL8169_DESC_OWN | RTL8169_BUFFER_SIZE;
        if (i == RTL8169_RX_COUNT - 1) cmd |= RTL8169_DESC_EOR;
        device->rx_ring[i].command  = cmd;
        device->rx_ring[i].vlan     = 0;
        device->rx_ring[i].low_buf  = (uint32_t)device->rx_buffer_phys[i];
        device->rx_ring[i].high_buf = (uint32_t)(device->rx_buffer_phys[i] >> 32);
    }
    for (size_t i = 0; i < RTL8169_TX_COUNT; i++) {
        device->tx_buffer_phys[i] = alloc_frames(1);
        if (!device->tx_buffer_phys[i]) return -ENOMEM;
        uint32_t cmd = 0;
        if (i == RTL8169_TX_COUNT - 1) cmd |= RTL8169_DESC_EOR;
        device->tx_ring[i].command  = cmd; /* OWN clear: available */
        device->tx_ring[i].vlan     = 0;
        device->tx_ring[i].low_buf  = (uint32_t)device->tx_buffer_phys[i];
        device->tx_ring[i].high_buf = (uint32_t)(device->tx_buffer_phys[i] >> 32);
    }
    dma_write_barrier();
    return 0;
}

static void rtl8169_program_hw(rtl8169_device_t *device)
{
    rtl8169_write16(device, RTL8169_REG_IMR, 0);
    rtl8169_write16(device, RTL8169_REG_ISR, 0xffff);
    rtl8169_write_flush(device);

    /* C+ command register must be programmed before the descriptor rings. */
    rtl8169_write16(device, RTL8169_REG_CPLUSCR, RTL8169_CPLUS_DAC);

    /* Descriptor ring base addresses (64-bit). */
    rtl8169_write32(device, RTL8169_REG_TNPDS, (uint32_t)device->tx_ring_phys);
    rtl8169_write32(device, RTL8169_REG_TNPDS + 4, (uint32_t)(device->tx_ring_phys >> 32));
    rtl8169_write32(device, RTL8169_REG_RDSAR, (uint32_t)device->rx_ring_phys);
    rtl8169_write32(device, RTL8169_REG_RDSAR + 4, (uint32_t)(device->rx_ring_phys >> 32));

    /* Unlock config registers. */
    rtl8169_write8(device, RTL8169_REG_9346CR, RTL8169_9346_UNLOCK);

    /* Receive configuration: accept everything, unlimited DMA/FIFO. */
    rtl8169_write32(device, RTL8169_REG_RCR,
                    RTL8169_RCR_AAP | RTL8169_RCR_APM | RTL8169_RCR_AM | RTL8169_RCR_AB |
                    RTL8169_RCR_MXDMA | RTL8169_RCR_RXFTH);

    /* Enable the transmitter before writing TCR (datasheet requirement). */
    rtl8169_write8(device, RTL8169_REG_CR, RTL8169_CR_TE);

    /* Transmit configuration: normal IFG, unlimited DMA burst. */
    rtl8169_write32(device, RTL8169_REG_TCR, RTL8169_TCR_IFG_NORMAL | RTL8169_TCR_MXDMA_UNLIMITED);

    rtl8169_write16(device, RTL8169_REG_RMS, RTL8169_BUFFER_SIZE);
    rtl8169_write8(device, RTL8169_REG_MTPS, 0x3b);

    /* Lock config registers. */
    rtl8169_write8(device, RTL8169_REG_9346CR, RTL8169_9346_LOCK);

    /* Enable RX and TX. */
    rtl8169_write8(device, RTL8169_REG_CR, RTL8169_CR_TE | RTL8169_CR_RE);
    rtl8169_write_flush(device);
}

static void rtl8169_update_link(rtl8169_device_t *device)
{
    int up = !!(rtl8169_read8(device, RTL8169_REG_PHYSTATUS) & RTL8169_PHYSTATUS_LINKSTS);
    if (up == device->link_up) return;
    device->link_up = up;
    device->stats.link_changes++;
    if (!up) plogk("rtl8169: %s: link down.\n", device->netdev.name);
    if (device->netdev_registered) {
        spin_lock(&device->netdev.lock);
        if (up && (device->netdev.flags & NETDEV_F_UP))
            device->netdev.flags |= NETDEV_F_RUNNING;
        else
            device->netdev.flags &= ~NETDEV_F_RUNNING;
        spin_unlock(&device->netdev.lock);
    }
}

static size_t rtl8169_tx_reclaim_locked(rtl8169_device_t *device, size_t budget)
{
    size_t reclaimed = 0;

    while (device->tx_used && reclaimed < budget) {
        volatile rtl8169_desc_t *desc = &device->tx_ring[device->tx_clean];
        dma_read_barrier();
        if (desc->command & RTL8169_DESC_OWN) break;
        /* The Tx status descriptor no longer carries the frame length
         * (bits 27-0 are reserved), so bytes are counted at submit time. */
        device->stats.tx_packets++;
        device->tx_clean = (device->tx_clean + 1) % RTL8169_TX_COUNT;
        device->tx_used--;
        reclaimed++;
    }
    return reclaimed;
}

static int rtl8169_rx_ready_locked(rtl8169_device_t *device)
{
    dma_read_barrier();
    return !(device->rx_ring[device->rx_next].command & RTL8169_DESC_OWN);
}

static int rtl8169_rx_ready(rtl8169_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&device->rx_lock);
    int      ready  = rtl8169_rx_ready_locked(device);
    spin_unlock_irqrestore(&device->rx_lock, rflags);
    return ready;
}

static int rtl8169_tx_ready_locked(rtl8169_device_t *device)
{
    dma_read_barrier();
    return device->tx_used && !(device->tx_ring[device->tx_clean].command & RTL8169_DESC_OWN);
}

static int rtl8169_tx_ready(rtl8169_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    int      ready  = rtl8169_tx_ready_locked(device);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
    return ready;
}

static int rtl8169_net_open(net_device_t *netdev)
{
    rtl8169_device_t *device = netdev_private(netdev);
    if (!device || !device->running) return -ENODEV;
    return 0;
}

static void rtl8169_net_stop(net_device_t *netdev)
{
    (void)netdev;
}

static int rtl8169_net_xmit(net_device_t *netdev, net_pbuf_t *packet)
{
    rtl8169_device_t *device = netdev_private(netdev);
    if (!packet) return -EINVAL;
    return rtl8169_transmit(device, packet->data, packet->length);
}

static int rtl8169_net_set_mtu(net_device_t *netdev, uint32_t mtu)
{
    (void)netdev;
    return mtu == RTL8169_MTU ? 0 : -EOPNOTSUPP;
}

static const netdev_ops_t rtl8169_netdev_ops = {
    .open    = rtl8169_net_open,
    .stop    = rtl8169_net_stop,
    .xmit    = rtl8169_net_xmit,
    .set_mtu = rtl8169_net_set_mtu,
};

int rtl8169_transmit(rtl8169_device_t *device, const void *packet, size_t length)
{
    if (!device || !packet || length == 0) return -EINVAL;
    if (length > RTL8169_MAX_FRAME_SIZE) {
        device->stats.tx_dropped++;
        device->stats.tx_errors++;
        return -EMSGSIZE;
    }
    if (!device->running) return -ENODEV;
    if (!device->link_up) return -ENETDOWN;

    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    rtl8169_tx_reclaim_locked(device, RTL8169_TX_RECLAIM_BUDGET);
    if (!device->running || device->stopping) {
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -ENODEV;
    }
    if (!device->link_up) {
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -ENETDOWN;
    }
    /* Keep one descriptor unused so equal head and tail always means empty. */
    if (device->tx_used == RTL8169_TX_COUNT - 1) {
        device->stats.tx_busy++;
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -EAGAIN;
    }

    uint16_t idx = device->tx_next;
    memcpy(phys_to_virt(device->tx_buffer_phys[idx]), packet, length);
    device->stats.tx_bytes += length;

    volatile rtl8169_desc_t *desc = &device->tx_ring[idx];
    uint32_t cmd = RTL8169_DESC_FS | RTL8169_DESC_LS | ((uint32_t)length & RTL8169_TX_LEN_MASK);
    if (idx == RTL8169_TX_COUNT - 1) cmd |= RTL8169_DESC_EOR;
    desc->command = cmd; /* OWN clear first so the update is ordered */
    dma_write_barrier();
    desc->command |= RTL8169_DESC_OWN;
    dma_write_barrier();

    device->tx_next = (idx + 1) % RTL8169_TX_COUNT;
    device->tx_used++;
    rtl8169_write8(device, RTL8169_REG_TPPOLL, RTL8169_TPPOLL_NPQ);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
    return 0;
}

size_t rtl8169_poll(rtl8169_device_t *device, size_t budget)
{
    size_t  done = 0;
    uint8_t frame[RTL8169_MAX_FRAME_SIZE];
    if (!device || !device->running) return 0;

    uint64_t rflags = spin_lock_irqsave(&device->rx_lock);
    if (!device->running || device->stopping) {
        spin_unlock_irqrestore(&device->rx_lock, rflags);
        return 0;
    }
    while (done < budget) {
        uint16_t                  idx  = device->rx_next;
        volatile rtl8169_desc_t  *desc = &device->rx_ring[idx];
        uint32_t                  cmd  = desc->command;
        dma_read_barrier();
        if (cmd & RTL8169_DESC_OWN) break;

        uint32_t length    = cmd & RTL8169_RX_LEN_MASK; /* includes the 4-byte CRC */
        int      complete  = (cmd & RTL8169_DESC_LS) && (cmd & RTL8169_DESC_FS);
        int      good      = complete && !(cmd & RTL8169_RX_ERROR_MASK) &&
                             length > RTL8169_CRC_LEN &&
                             length <= RTL8169_BUFFER_SIZE &&
                             length - RTL8169_CRC_LEN <= RTL8169_MAX_FRAME_SIZE;
        size_t   frame_length = good ? length - RTL8169_CRC_LEN : 0;

        if (!good) {
            device->stats.rx_errors++;
        } else {
            memcpy(frame, phys_to_virt(device->rx_buffer_phys[idx]), frame_length);
        }

        uint32_t rearm = RTL8169_DESC_OWN | RTL8169_BUFFER_SIZE;
        if (idx == RTL8169_RX_COUNT - 1) rearm |= RTL8169_DESC_EOR;
        desc->command = rearm;
        dma_write_barrier();
        device->rx_next = (idx + 1) % RTL8169_RX_COUNT;
        done++;

        if (good) {
            spin_unlock_irqrestore(&device->rx_lock, rflags);
            net_pbuf_t *packet = net_pbuf_from(frame, frame_length, NET_PBUF_HEADROOM);
            if (!packet) {
                static uint64_t last_log;
                if (sched_ticks() - last_log >= 1000) {
                    plogk("rtl8169: %s: RX frame allocation failed.\n", device->netdev.name);
                    last_log = sched_ticks();
                }
                device->stats.rx_dropped++;
            } else {
                if (netdev_rx(&device->netdev, packet))
                    device->stats.rx_dropped++;
                else {
                    device->stats.rx_packets++;
                    device->stats.rx_bytes += frame_length;
                }
            }
            rflags = spin_lock_irqsave(&device->rx_lock);
            if (!device->running) break;
        }
    }
    spin_unlock_irqrestore(&device->rx_lock, rflags);
    return done;
}

static void rtl8169_process_work(rtl8169_device_t *device, uint32_t cause)
{
    if (cause & RTL8169_ISR_LINKCHG) rtl8169_update_link(device);
    if (cause & RTL8169_ISR_RER) device->stats.rx_errors++;
    if (cause & (RTL8169_ISR_RDU | RTL8169_ISR_FOVW)) {
        device->stats.rx_errors++;
        device->stats.rx_overruns++;
        plogk("rtl8169: %s: RX descriptor/FIFO overrun.\n", device->netdev.name);
    }
    if (cause & RTL8169_ISR_TER) device->stats.tx_errors++;
    if (cause & RTL8169_ISR_TDU) {
        device->stats.tx_errors++;
        plogk("rtl8169: %s: TX descriptor unavailable.\n", device->netdev.name);
    }
    if ((cause & RTL8169_RX_INT_MASK) || rtl8169_rx_ready(device)) (void)rtl8169_poll(device, RTL8169_WORK_BUDGET);

    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    if ((cause & RTL8169_ISR_TOK) || rtl8169_tx_ready_locked(device)) rtl8169_tx_reclaim_locked(device, RTL8169_TX_RECLAIM_BUDGET);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
}

static void rtl8169_worker(void *arg)
{
    rtl8169_device_t *device = arg;

    for (;;) {
        uint64_t rflags = spin_lock_irqsave(&device->work_lock);
        while (!device->work_pending && !device->stopping) {
            wait_queue_prepare(&device->work_wait);
            spin_unlock_irqrestore(&device->work_lock, rflags);
            wait_queue_sleep();
            rflags = spin_lock_irqsave(&device->work_lock);
        }
        if (device->stopping) {
            device->worker_exited  = 1;
            device->worker_started = 0;
            wait_queue_wake_all(&device->exit_wait);
            spin_unlock_irqrestore(&device->work_lock, rflags);
            return;
        }
        uint32_t cause             = device->work_pending;
        uint32_t interrupts        = device->interrupts_pending;
        device->work_pending       = 0;
        device->interrupts_pending = 0;
        spin_unlock_irqrestore(&device->work_lock, rflags);

        device->stats.interrupts += interrupts;
        rtl8169_process_work(device, cause);

        rflags = spin_lock_irqsave(&device->work_lock);
        if (!device->stopping && !device->work_pending && !rtl8169_rx_ready(device) && !rtl8169_tx_ready(device)) {
            rtl8169_write16(device, RTL8169_REG_IMR, RTL8169_INT_MASK);
            rtl8169_write_flush(device);
            uint16_t status = rtl8169_read16(device, RTL8169_REG_ISR) & RTL8169_INT_MASK;
            if (status) {
                rtl8169_write16(device, RTL8169_REG_IMR, 0);
                rtl8169_write16(device, RTL8169_REG_ISR, status);
                device->work_pending |= status;
            }
        } else if (!device->stopping && !device->work_pending) {
            device->work_pending = RTL8169_ISR_ROK | RTL8169_ISR_TOK;
        }
        int more = !device->stopping && device->work_pending;
        spin_unlock_irqrestore(&device->work_lock, rflags);
        if (more) sched_yield();
    }
}

static int rtl8169_start_worker(rtl8169_device_t *device)
{
    if (device->worker_started) return 0;

    uint64_t rflags        = spin_lock_irqsave(&device->work_lock);
    device->worker_started = 1;
    device->worker_exited  = 0;
    device->work_pending   = RTL8169_WORK_INITIAL;
    spin_unlock_irqrestore(&device->work_lock, rflags);

    task_t *worker = kthread_create("rtl8169-rx", rtl8169_worker, device);
    if (!worker) {
        rflags                 = spin_lock_irqsave(&device->work_lock);
        device->worker_started = 0;
        device->work_pending   = 0;
        spin_unlock_irqrestore(&device->work_lock, rflags);
        return -ENOMEM;
    }
    device->worker_task = worker;
    return 0;
}

static void rtl8169_interrupt_device(rtl8169_device_t *device)
{
    uint16_t status = rtl8169_read16(device, RTL8169_REG_ISR);
    if (!(status & RTL8169_INT_MASK)) return;

    rtl8169_write16(device, RTL8169_REG_IMR, 0);
    rtl8169_write16(device, RTL8169_REG_ISR, status);
    rtl8169_write_flush(device);
    uint64_t rflags = spin_lock_irqsave(&device->work_lock);
    if (device->running && device->worker_started && !device->stopping) {
        device->work_pending |= status & RTL8169_INT_MASK;
        device->interrupts_pending++;
        wait_queue_wake_one(&device->work_wait);
    }
    spin_unlock_irqrestore(&device->work_lock, rflags);
}

static void rtl8169_interrupt_slot(size_t slot, void *frame)
{
    (void)frame;
    uint64_t         rflags = spin_lock_irqsave(&rtl8169_irq_lock);
    rtl8169_device_t *device = rtl8169_irq_slots[slot];
    if (device && !device->stopping)
        device->irq_active++;
    else
        device = NULL;
    spin_unlock_irqrestore(&rtl8169_irq_lock, rflags);

    if (device) {
        rtl8169_interrupt_device(device);
        rflags = spin_lock_irqsave(&rtl8169_irq_lock);
        device->irq_active--;
        spin_unlock_irqrestore(&rtl8169_irq_lock, rflags);
    }
    send_eoi();
}

#define RTL8169_IRQ_WRAPPERS(n)                                                   \
    static void rtl8169_legacy_interrupt_##n(void *frame)                         \
    {                                                                             \
        rtl8169_interrupt_slot(n, frame);                                         \
    }                                                                             \
    INTERRUPT_BEGIN static void rtl8169_idt_interrupt_##n(interrupt_frame_t *frame) \
    {                                                                             \
        rtl8169_interrupt_slot(n, frame);                                         \
    }                                                                             \
    INTERRUPT_END

RTL8169_IRQ_WRAPPERS(0)
RTL8169_IRQ_WRAPPERS(1)
RTL8169_IRQ_WRAPPERS(2)
RTL8169_IRQ_WRAPPERS(3)
RTL8169_IRQ_WRAPPERS(4)
RTL8169_IRQ_WRAPPERS(5)
RTL8169_IRQ_WRAPPERS(6)
RTL8169_IRQ_WRAPPERS(7)

static const net_irq_handler_fn rtl8169_legacy_irq_handlers[RTL8169_MAX_DEVICES] = {
    rtl8169_legacy_interrupt_0, rtl8169_legacy_interrupt_1, rtl8169_legacy_interrupt_2, rtl8169_legacy_interrupt_3,
    rtl8169_legacy_interrupt_4, rtl8169_legacy_interrupt_5, rtl8169_legacy_interrupt_6, rtl8169_legacy_interrupt_7,
};

static void *const rtl8169_idt_irq_handlers[RTL8169_MAX_DEVICES] = {
    (void *)rtl8169_idt_interrupt_0, (void *)rtl8169_idt_interrupt_1, (void *)rtl8169_idt_interrupt_2, (void *)rtl8169_idt_interrupt_3,
    (void *)rtl8169_idt_interrupt_4, (void *)rtl8169_idt_interrupt_5, (void *)rtl8169_idt_interrupt_6, (void *)rtl8169_idt_interrupt_7,
};

static int rtl8169_setup_interrupt(rtl8169_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&rtl8169_irq_lock);
    size_t   slot;
    for (slot = 0; slot < RTL8169_MAX_DEVICES; slot++)
        if (!rtl8169_irq_slots[slot]) break;
    if (slot == RTL8169_MAX_DEVICES) {
        spin_unlock_irqrestore(&rtl8169_irq_lock, rflags);
        return -ENOSPC;
    }
    device->irq_slot      = (uint8_t)slot;
    rtl8169_irq_slots[slot] = device;
    spin_unlock_irqrestore(&rtl8169_irq_lock, rflags);

    pci_msi_init(device->pci);
    device->vector = pci_enable_msi(device->pci);
    if (device->vector >= 0) {
        register_interrupt_handler((uint16_t)device->vector, rtl8169_idt_irq_handlers[slot], 0, 0x8e);
        device->using_msi = 1;
        return 0;
    }

    device->irq = (uint8_t)pci_get_irq(device->pci);
    if (device->irq == 0 || device->irq == 0xff) goto fail;
    if (net_irq_claim_legacy && net_irq_release_legacy) {
        if (net_irq_claim_legacy(device->irq, rtl8169_legacy_irq_handlers[slot])) goto fail;
    } else {
        /*
         * Some platforms expose only INTx and this kernel may be built
         * without a shared legacy-IRQ dispatcher. Install an exclusive
         * fallback route so the device is not rejected before its RX
         * worker can start.
         */
        device->vector = IRQ_0 + device->irq;
        register_interrupt_handler((uint16_t)device->vector, rtl8169_idt_irq_handlers[slot], 0, 0x8e);
        ioapic_routing_t routing = {(uint8_t)device->vector, device->irq};
        ioapic_add(&routing);
        device->using_direct_legacy = 1;
    }
    device->using_legacy = 1;
    return 0;

fail:
    rflags                  = spin_lock_irqsave(&rtl8169_irq_lock);
    rtl8169_irq_slots[slot] = NULL;
    spin_unlock_irqrestore(&rtl8169_irq_lock, rflags);
    return -ENODEV;
}

static void rtl8169_release_interrupt(rtl8169_device_t *device)
{
    if (!device->using_msi && !device->using_legacy) return;
    if (device->mmio) {
        rtl8169_write16(device, RTL8169_REG_IMR, 0);
        rtl8169_write16(device, RTL8169_REG_ISR, 0xffff);
        rtl8169_write_flush(device);
    }
    uint64_t rflags                   = spin_lock_irqsave(&rtl8169_irq_lock);
    rtl8169_irq_slots[device->irq_slot] = NULL;
    spin_unlock_irqrestore(&rtl8169_irq_lock, rflags);

    if (device->using_msi) pci_disable_msi(device->pci);
    if (device->using_legacy && !device->using_direct_legacy && net_irq_release_legacy)
        net_irq_release_legacy(device->irq, rtl8169_legacy_irq_handlers[device->irq_slot]);
    for (;;) {
        rflags     = spin_lock_irqsave(&rtl8169_irq_lock);
        int active = device->irq_active != 0;
        spin_unlock_irqrestore(&rtl8169_irq_lock, rflags);
        if (!active) break;
        __asm__ volatile("pause" ::: "memory");
    }
    device->using_msi = device->using_legacy = device->using_direct_legacy = 0;
}

static int rtl8169_netdev_name(char *name, size_t size)
{
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        char          candidate[NETDEV_NAME_MAX];
        net_device_t *existing;
        (void)snprintf(candidate, sizeof(candidate), "eth%u", i);
        existing = netdev_get_by_name(candidate);
        if (existing) {
            netdev_put(existing);
            continue;
        }
        strncpy(name, candidate, size - 1);
        name[size - 1] = '\0';
        return 0;
    }
    return -ENOSPC;
}

static void rtl8169_destroy(rtl8169_device_t *device)
{
    if (!device) return;
    uint64_t rflags  = spin_lock_irqsave(&device->work_lock);
    device->stopping = 1;
    device->running  = 0;
    spin_unlock_irqrestore(&device->work_lock, rflags);
    rtl8169_release_interrupt(device);
    if (device->worker_task) {
        rflags = spin_lock_irqsave(&device->work_lock);
        wait_queue_wake_all(&device->work_wait);
        while (!device->worker_exited) {
            wait_queue_prepare(&device->exit_wait);
            spin_unlock_irqrestore(&device->work_lock, rflags);
            wait_queue_sleep();
            rflags = spin_lock_irqsave(&device->work_lock);
        }
        spin_unlock_irqrestore(&device->work_lock, rflags);
        while (__atomic_load_n(&device->worker_task->state, __ATOMIC_ACQUIRE) != TASK_ZOMBIE
               || __atomic_load_n(&device->worker_task->on_cpu, __ATOMIC_ACQUIRE))
            sched_yield();
        task_free(device->worker_task);
        device->worker_task = NULL;
    }
    if (device->netdev_registered) {
        netdev_unregister(&device->netdev);
        device->netdev_registered = 0;
    }
    rflags = spin_lock_irqsave(&device->rx_lock);
    spin_unlock_irqrestore(&device->rx_lock, rflags);
    rflags = spin_lock_irqsave(&device->tx_lock);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
    if (device->mmio) {
        rtl8169_write16(device, RTL8169_REG_IMR, 0);
        rtl8169_write8(device, RTL8169_REG_CR, 0);
        rtl8169_write_flush(device);
        msleep(10);
    }
    if (device->pci) {
        uint16_t command = pci_read_command_status(device->pci) & 0xffff;
        pci_write_command_status(device->pci, command & ~(1u << 2));
        (void)pci_read_command_status(device->pci);
    }
    dma_full_barrier();
    rtl8169_free_dma(device);
    if (device->pci) pci_write_command_status(device->pci, device->saved_command);
    free(device);
}

int rtl8169_probe(pci_device_cache_t *pci)
{
    if (!pci || rtl8169_device_count >= RTL8169_MAX_DEVICES) return -ENOSPC;
    const rtl8169_id_t *id = rtl8169_match((uint16_t)pci->vendor_id, (uint16_t)pci->device_id);
    if (!id) return -ENODEV;
    for (rtl8169_device_t *it = rtl8169_devices; it; it = it->next)
        if (it->pci == pci) return -EEXIST;

    rtl8169_device_t *device = malloc(sizeof(*device));
    if (!device) return -ENOMEM;
    memset(device, 0, sizeof(*device));
    device->pci           = pci;
    device->device_id     = id->device;
    device->features      = 0;
    device->vector        = -1;
    device->saved_command = pci_read_command_status(pci) & 0xffff;
    wait_queue_init(&device->work_wait);
    wait_queue_init(&device->exit_wait);
    const char *stage = "BAR mapping";

    /* BAR sizing writes all ones, so memory and I/O decoding must be off. */
    pci_write_command_status(pci, device->saved_command & ~((1u << 1) | (1u << 2)));
    int ret = rtl8169_map_bar(device);
    if (ret) goto fail;
    pci_write_command_status(pci, device->saved_command | (1u << 1) | (1u << 2));
    stage = "reset";
    ret = rtl8169_reset(device);
    if (ret) goto fail;
    rtl8169_read_mac(device);
    stage = "DMA rings";
    ret   = rtl8169_alloc_dma(device);
    if (ret) goto fail;

    rtl8169_program_hw(device);
    stage = "interrupt setup";
    ret   = rtl8169_setup_interrupt(device);
    if (ret) goto fail;

    char netdev_name[NETDEV_NAME_MAX];
    stage = "netdev initialization";
    ret   = rtl8169_netdev_name(netdev_name, sizeof(netdev_name));
    if (ret) goto fail;
    ret = netdev_init(&device->netdev, netdev_name, &rtl8169_netdev_ops, device);
    if (ret) goto fail;
    memcpy(device->netdev.address, device->mac, sizeof(device->mac));
    device->netdev.mtu   = RTL8169_MTU;
    device->netdev.flags = NETDEV_F_BROADCAST;
    stage                = "netdev registration";
    ret                  = netdev_register(&device->netdev);
    if (ret) goto fail;
    device->netdev_registered = 1;
    device->next              = rtl8169_devices;
    rtl8169_devices           = device;
    device->running           = 1;
    rtl8169_device_count++;
    device->link_up = !!(rtl8169_read8(device, RTL8169_REG_PHYSTATUS) & RTL8169_PHYSTATUS_LINKSTS);
    stage           = "netdev activation";
    ret             = netdev_set_up(&device->netdev, 1);
    if (ret) goto fail_linked;
    if (!device->link_up) {
        spin_lock(&device->netdev.lock);
        device->netdev.flags &= ~NETDEV_F_RUNNING;
        spin_unlock(&device->netdev.lock);
    }
    (void)rtl8169_read16(device, RTL8169_REG_ISR);
    if (rtl8169_scheduler_ready) {
        stage = "worker startup";
        ret   = rtl8169_start_worker(device);
        if (ret) goto fail_linked;
    }
    return 0;

fail_linked:
    if (rtl8169_devices == device) rtl8169_devices = device->next;
    if (rtl8169_device_count) rtl8169_device_count--;

fail:
    plogk("rtl8169: Probe failed during %s (%d).\n", stage, ret);
    rtl8169_destroy(device);
    return ret;
}

int rtl8169_init(void)
{
#if !CONFIG_RTL8169
    return 0;
#endif
    int                  found = 0;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return -ENODEV;
    for (pci_device_cache_t *pci = cache->head; pci; pci = pci->next) {
        if (!rtl8169_match((uint16_t)pci->vendor_id, (uint16_t)pci->device_id)) continue;
        if (!rtl8169_probe(pci)) found++;
    }
    return found ? found : -ENODEV;
}

int rtl8169_start_workers(void)
{
#if !CONFIG_RTL8169
    return 0;
#endif
    int started = 0;
    int failed  = 0;

    rtl8169_scheduler_ready = 1;
    rtl8169_device_t **link = &rtl8169_devices;
    while (*link) {
        rtl8169_device_t *device = *link;
        if (device->worker_started || !rtl8169_start_worker(device)) {
            started++;
            link = &device->next;
            continue;
        }
        failed = 1;
        plogk("rtl8169: %s: worker startup failed.\n", device->netdev.name);
        *link = device->next;
        rtl8169_device_count--;
        rtl8169_destroy(device);
    }
    return started ? started : (failed ? -ENOMEM : -ENODEV);
}

void rtl8169_shutdown(void)
{
    while (rtl8169_devices) {
        rtl8169_device_t *device = rtl8169_devices;
        rtl8169_devices          = device->next;
        rtl8169_device_count--;
        rtl8169_destroy(device);
    }
}

int rtl8169_link_up(const rtl8169_device_t *device)
{
    return device && device->link_up;
}

const uint8_t *rtl8169_mac_address(const rtl8169_device_t *device)
{
    return device ? device->mac : NULL;
}

const rtl8169_stats_t *rtl8169_get_stats(const rtl8169_device_t *device)
{
    return device ? &device->stats : NULL;
}

rtl8169_device_t *rtl8169_first_device(void)
{
    return rtl8169_devices;
}

rtl8169_device_t *rtl8169_next_device(rtl8169_device_t *device)
{
    return device ? device->next : NULL;
}
