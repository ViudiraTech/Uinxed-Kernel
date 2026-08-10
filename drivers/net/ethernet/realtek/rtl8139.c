/*
 *
 *      rtl8139.c
 *      Realtek RTL8139 network controller driver
 *
 *      2026/8/9 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/idt.h>
#include <drivers/firmware/apic.h>
#include <drivers/net/ethernet/realtek/rtl8139.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <net/core/netdev.h>
#include <net/core/pbuf.h>
#include <process/sched.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#define RTL8139_MAX_DEVICES       8
#define RTL8139_TX_COUNT          4 // four hardware transmit descriptors
#define RTL8139_RX_BUF_IDX        2 // RCR receive ring length: 32K + 16
#define RTL8139_RX_BUF_SIZE       (8192u << RTL8139_RX_BUF_IDX)
#define RTL8139_RX_BUF_FRAMES     ((RTL8139_RX_BUF_SIZE + 16 + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE)
#define RTL8139_MAX_FRAME_SIZE    (RTL8139_MTU + 18)
#define RTL8139_CRC_LEN           4  // RX frame length reported by the chip includes CRC
#define RTL8139_ETH_ZLEN          60 // minimum frame octets without CRC; chip has no auto-pad
#define RTL8139_WORK_BUDGET       64
#define RTL8139_TX_RECLAIM_BUDGET RTL8139_TX_COUNT
#define RTL8139_RESET_POLL        1000

/*
 * Register map (RTL8139D datasheet Rev 1.11).
 * The classic RTL8139 exposes these registers through PCI I/O space.
 */
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

#define RTL8139_INT_MASK \
    (RTL8139_ISR_ROK | RTL8139_ISR_RER | RTL8139_ISR_TOK | RTL8139_ISR_TER | RTL8139_ISR_RXOVW | RTL8139_ISR_PUN | RTL8139_ISR_FOVW)
#define RTL8139_RX_INT_MASK  (RTL8139_ISR_ROK | RTL8139_ISR_RER | RTL8139_ISR_RXOVW | RTL8139_ISR_FOVW)
#define RTL8139_WORK_INITIAL (RTL8139_ISR_ROK | RTL8139_ISR_TOK)

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
#define RTL8139_RX_ROK        (1u << 0) // receive OK
#define RTL8139_RX_FAE        (1u << 1) // frame alignment error
#define RTL8139_RX_CRC        (1u << 2) // CRC error
#define RTL8139_RX_LONG       (1u << 3) // frame exceeds 4K bytes
#define RTL8139_RX_RUNT       (1u << 4) // runt packet
#define RTL8139_RX_ISE        (1u << 5) // invalid symbol error
#define RTL8139_RX_ERROR_MASK (RTL8139_RX_FAE | RTL8139_RX_CRC | RTL8139_RX_LONG | RTL8139_RX_RUNT | RTL8139_RX_ISE)

/*
 * Device IDs that use the classic RTL8139 transmit-descriptor and
 * receive-ring register layout.
 */
typedef struct {
        uint16_t vendor;
        uint16_t device;
} rtl8139_id_t;

static const rtl8139_id_t rtl8139_ids[] = {
    {0x10ec, 0x8129}, // RTL8129
    {0x10ec, 0x8139}, // RTL8139 / RTL8139D
    {0x10ec, 0x8138}, // RTL8139B
    {0x1113, 0x1211}, // Accton EN-1207D / SMC1211TX
    {0x1186, 0x1300}, // D-Link DFE-538TX
    {0x018a, 0x0106}, // LevelOne FPC-0106Tx
    {0x021b, 0x8139}, // Compaq HNE-300
};

typedef struct rtl8139_device {
        pci_device_cache_t    *pci;
        uint16_t               ioaddr;
        uint16_t               device_id;
        uint16_t               saved_command;
        uint8_t                mac[6];
        uint8_t                irq;
        int                    vector;
        int                    using_legacy;
        int                    using_direct_legacy;
        int                    running;
        int                    stopping;
        int                    link_up;
        uint8_t                irq_slot;
        volatile uint32_t      irq_active;
        uint32_t               work_pending;
        uint32_t               interrupts_pending;
        int                    worker_started;
        int                    worker_exited;
        task_t                *worker_task;
        wait_queue_t           work_wait;
        wait_queue_t           exit_wait;
        spinlock_t             work_lock;
        volatile uint8_t      *rx_ring;
        uint64_t               rx_ring_phys;
        uint64_t               tx_buffer_phys[RTL8139_TX_COUNT];
        uint32_t               cur_rx;
        uint8_t                tx_next;
        uint8_t                tx_clean;
        uint8_t                tx_used;
        spinlock_t             rx_lock;
        spinlock_t             tx_lock;
        rtl8139_stats_t        stats;
        net_device_t           netdev;
        int                    netdev_registered;
        struct rtl8139_device *next;
} rtl8139_device_t;

static rtl8139_device_t *rtl8139_devices;
static size_t            rtl8139_device_count;
static rtl8139_device_t *rtl8139_irq_slots[RTL8139_MAX_DEVICES];
static spinlock_t        rtl8139_irq_lock;
static int               rtl8139_scheduler_ready;

static inline uint8_t rtl8139_read8(const rtl8139_device_t *device, uint32_t reg)
{
    return inb((uint16_t)(device->ioaddr + reg));
}

static inline uint16_t rtl8139_read16(const rtl8139_device_t *device, uint32_t reg)
{
    return inw((uint16_t)(device->ioaddr + reg));
}

static inline uint32_t rtl8139_read32(const rtl8139_device_t *device, uint32_t reg)
{
    return inl((uint16_t)(device->ioaddr + reg));
}

static inline void rtl8139_write8(rtl8139_device_t *device, uint32_t reg, uint8_t value)
{
    outb((uint16_t)(device->ioaddr + reg), value);
}

static inline void rtl8139_write16(rtl8139_device_t *device, uint32_t reg, uint16_t value)
{
    outw((uint16_t)(device->ioaddr + reg), value);
}

static inline void rtl8139_write32(rtl8139_device_t *device, uint32_t reg, uint32_t value)
{
    outl((uint16_t)(device->ioaddr + reg), value);
}

static inline void rtl8139_write_flush(rtl8139_device_t *device)
{
    (void)rtl8139_read8(device, RTL8139_REG_CR);
}

static const rtl8139_id_t *rtl8139_match(uint16_t vendor, uint16_t device)
{
    for (size_t i = 0; i < sizeof(rtl8139_ids) / sizeof(rtl8139_ids[0]); i++)
        if (rtl8139_ids[i].vendor == vendor && rtl8139_ids[i].device == device) return &rtl8139_ids[i];
    return NULL;
}

static int rtl8139_valid_mac(const uint8_t mac[6])
{
    uint8_t any = 0;
    uint8_t all = 0xff;
    for (size_t i = 0; i < 6; i++) {
        any |= mac[i];
        all &= mac[i];
    }
    return any != 0 && all != 0xff && !(mac[0] & 1);
}

static int rtl8139_get_ioaddr(rtl8139_device_t *device)
{
    uint32_t port = pci_get_port_base(device->pci);
    if (!port || port > 0xffff) {
        plogk("rtl8139: %04x:%04x: No usable I/O port BAR.\n", (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
        return -ENODEV;
    }
    device->ioaddr = (uint16_t)port;
    return 0;
}

static int rtl8139_reset(rtl8139_device_t *device)
{
    rtl8139_write8(device, RTL8139_REG_CR, RTL8139_CR_RESET);
    rtl8139_write_flush(device);
    /*
     * The RTL8139 clears the reset bit once the reset completes, but some
     * implementations (e.g. QEMU) keep the bit asserted indefinitely, so
     * poll for a bounded time and continue regardless.
     */
    for (uint32_t i = 0; i < RTL8139_RESET_POLL; i++) {
        if (!(rtl8139_read8(device, RTL8139_REG_CR) & RTL8139_CR_RESET)) return 0;
    }
    plogk("rtl8139: %04x:%04x: Reset bit did not clear, continuing.\n", (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
    return 0;
}

static void rtl8139_read_mac(rtl8139_device_t *device)
{
    for (size_t i = 0; i < 6; i++) device->mac[i] = rtl8139_read8(device, RTL8139_REG_IDR0 + (uint32_t)i);
    if (!rtl8139_valid_mac(device->mac)) {
        plogk("rtl8139: %04x:%04x: Invalid MAC address, using fallback.\n", (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
        for (size_t i = 0; i < 6; i++) device->mac[i] = i;
        device->mac[0] &= ~1u; // ensure a unicast, locally administered address
        device->mac[0] |= 2u;
    }
}

static void rtl8139_free_dma(rtl8139_device_t *device)
{
    for (size_t i = 0; i < RTL8139_TX_COUNT; i++) {
        if (device->tx_buffer_phys[i]) free_frames(device->tx_buffer_phys[i], 1);
        device->tx_buffer_phys[i] = 0;
    }
    if (device->rx_ring_phys) free_frames(device->rx_ring_phys, RTL8139_RX_BUF_FRAMES);
    device->rx_ring_phys = 0;
    device->rx_ring      = NULL;
}

static int rtl8139_alloc_dma(rtl8139_device_t *device)
{
    device->rx_ring_phys = alloc_frames(RTL8139_RX_BUF_FRAMES);
    if (!device->rx_ring_phys) {
        plogk("rtl8139: %04x:%04x: RX ring allocation failed.\n", (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
        return -ENOMEM;
    }
    device->rx_ring = (volatile uint8_t *)phys_to_virt(device->rx_ring_phys);
    memset((void *)device->rx_ring, 0, (size_t)RTL8139_RX_BUF_FRAMES * PAGE_4K_SIZE);

    for (size_t i = 0; i < RTL8139_TX_COUNT; i++) {
        device->tx_buffer_phys[i] = alloc_frames(1);
        if (!device->tx_buffer_phys[i]) {
            plogk("rtl8139: %04x:%04x: TX buffer allocation failed.\n", (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
            return -ENOMEM;
        }
    }
    dma_write_barrier();
    return 0;
}

static void rtl8139_program_hw(rtl8139_device_t *device)
{
    rtl8139_write16(device, RTL8139_REG_IMR, 0);
    rtl8139_write16(device, RTL8139_REG_ISR, 0xffff);
    rtl8139_write_flush(device);

    /* Unlock the config registers to restore the MAC and program RBSTART. */
    rtl8139_write8(device, RTL8139_REG_9346CR, RTL8139_9346_UNLOCK);

    /* IDR0-5 are written with 4-byte accesses. */
    rtl8139_write32(device, RTL8139_REG_IDR0,
                    (uint32_t)(device->mac[0] | (device->mac[1] << 8) | (device->mac[2] << 16) | (device->mac[3] << 24)));
    rtl8139_write32(device, RTL8139_REG_IDR0 + 4, (uint32_t)(device->mac[4] | (device->mac[5] << 8)));

    /* Receive ring base address (dword-aligned). */
    rtl8139_write32(device, RTL8139_REG_RBSTART, (uint32_t)device->rx_ring_phys);

    rtl8139_write8(device, RTL8139_REG_9346CR, RTL8139_9346_LOCK);

    device->cur_rx = 0;
    rtl8139_write16(device, RTL8139_REG_CAPR, (uint16_t)(device->cur_rx - 16));

    /* Enable the transmitter before writing TCR (QEMU enforces this). */
    rtl8139_write8(device, RTL8139_REG_CR, RTL8139_CR_TE | RTL8139_CR_RE);

    /* Transmit configuration: standard IFG, 1024-byte DMA bursts. */
    rtl8139_write32(device, RTL8139_REG_TCR, RTL8139_TCR_IFG_NORMAL | ((uint32_t)RTL8139_TCR_MXDMA << 8));

    /* Receive configuration: physical/multicast/broadcast, 32K ring. */
    rtl8139_write32(device, RTL8139_REG_RCR,
                    ((uint32_t)RTL8139_RCR_RXFTH << 13) | ((uint32_t)RTL8139_RX_BUF_IDX << 11) | ((uint32_t)RTL8139_RCR_MXDMA << 8)
                        | RTL8139_RCR_APM | RTL8139_RCR_AM | RTL8139_RCR_AB);

    rtl8139_write8(device, RTL8139_REG_CR, RTL8139_CR_TE | RTL8139_CR_RE);
    rtl8139_write_flush(device);
}

static void rtl8139_update_link(rtl8139_device_t *device)
{
    int up = !!(rtl8139_read16(device, RTL8139_REG_BMSR) & RTL8139_BMSR_LINK);
    if (up == device->link_up) return;
    device->link_up = up;
    device->stats.link_changes++;
    if (device->netdev_registered) {
        spin_lock(&device->netdev.lock);
        if (up && (device->netdev.flags & NETDEV_F_UP))
            device->netdev.flags |= NETDEV_F_RUNNING;
        else
            device->netdev.flags &= ~NETDEV_F_RUNNING;
        spin_unlock(&device->netdev.lock);
    }
}

static size_t rtl8139_tx_reclaim_locked(rtl8139_device_t *device, size_t budget)
{
    size_t reclaimed = 0;

    while (device->tx_used && reclaimed < budget) {
        uint32_t t = rtl8139_read32(device, RTL8139_REG_TSD0 + (uint32_t)device->tx_clean * 4);
        if (!(t & RTL8139_TX_OWN)) break;
        if (t & RTL8139_TX_TOK)
            device->stats.tx_packets++;
        else {
            device->stats.tx_errors++;
            plogk("rtl8139: %s: TX descriptor error (status=%#x)\n", device->netdev.name, (unsigned)t);
        }
        device->tx_clean = (device->tx_clean + 1) % RTL8139_TX_COUNT;
        device->tx_used--;
        reclaimed++;
    }
    return reclaimed;
}

static int rtl8139_rx_ready_locked(rtl8139_device_t *device)
{
    return !(rtl8139_read8(device, RTL8139_REG_CR) & RTL8139_CR_BUFE);
}

static int rtl8139_rx_ready(rtl8139_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&device->rx_lock);
    int      ready  = rtl8139_rx_ready_locked(device);
    spin_unlock_irqrestore(&device->rx_lock, rflags);
    return ready;
}

static int rtl8139_tx_ready_locked(rtl8139_device_t *device)
{
    return device->tx_used && (rtl8139_read32(device, RTL8139_REG_TSD0 + (uint32_t)device->tx_clean * 4) & RTL8139_TX_OWN);
}

static int rtl8139_tx_ready(rtl8139_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    int      ready  = rtl8139_tx_ready_locked(device);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
    return ready;
}

static int rtl8139_net_open(net_device_t *netdev)
{
    rtl8139_device_t *device = netdev_private(netdev);
    if (!device || !device->running) return -ENODEV;
    return 0;
}

static void rtl8139_net_stop(net_device_t *netdev)
{
    (void)netdev;
}

static int rtl8139_net_xmit(net_device_t *netdev, net_pbuf_t *packet)
{
    rtl8139_device_t *device = netdev_private(netdev);
    if (!packet) return -EINVAL;
    return rtl8139_transmit(device, packet->data, packet->length);
}

static int rtl8139_net_set_mtu(net_device_t *netdev, uint32_t mtu)
{
    (void)netdev;
    return mtu == RTL8139_MTU ? 0 : -EOPNOTSUPP;
}

static const netdev_ops_t rtl8139_netdev_ops = {
    .open    = rtl8139_net_open,
    .stop    = rtl8139_net_stop,
    .xmit    = rtl8139_net_xmit,
    .set_mtu = rtl8139_net_set_mtu,
};

int rtl8139_transmit(rtl8139_device_t *device, const void *packet, size_t length)
{
    if (!device || !packet || length == 0) return -EINVAL;
    if (length > RTL8139_MAX_FRAME_SIZE) {
        device->stats.tx_dropped++;
        device->stats.tx_errors++;
        plogk("rtl8139: %s: Dropping oversize TX frame (%zu bytes)\n", device->netdev.name, length);
        return -EMSGSIZE;
    }
    if (!device->running) return -ENODEV;
    if (!device->link_up) return -ENETDOWN;

    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    rtl8139_tx_reclaim_locked(device, RTL8139_TX_RECLAIM_BUDGET);
    if (!device->running || device->stopping) {
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -ENODEV;
    }
    if (!device->link_up) {
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -ENETDOWN;
    }
    if (device->tx_used == RTL8139_TX_COUNT) {
        device->stats.tx_busy++;
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -EAGAIN;
    }

    uint8_t  idx    = device->tx_next;
    uint8_t *buffer = phys_to_virt(device->tx_buffer_phys[idx]);
    memcpy(buffer, packet, length);
    if (length < RTL8139_ETH_ZLEN) {
        memset(buffer + length, 0, RTL8139_ETH_ZLEN - length);
        length = RTL8139_ETH_ZLEN;
    }
    device->stats.tx_bytes += length;

    /* Fill the start address, then the status to clear OWN and start the DMA. */
    rtl8139_write32(device, RTL8139_REG_TSAD0 + (uint32_t)idx * 4, (uint32_t)device->tx_buffer_phys[idx]);
    rtl8139_write32(device, RTL8139_REG_TSD0 + (uint32_t)idx * 4, ((uint32_t)RTL8139_TX_ERTXTH << 16) | (length & RTL8139_TX_LEN_MASK));

    device->tx_next = (idx + 1) % RTL8139_TX_COUNT;
    device->tx_used++;
    spin_unlock_irqrestore(&device->tx_lock, rflags);
    return 0;
}

size_t rtl8139_poll(rtl8139_device_t *device, size_t budget)
{
    size_t  done = 0;
    uint8_t frame[RTL8139_MAX_FRAME_SIZE];
    if (!device || !device->running) return 0;

    uint64_t rflags = spin_lock_irqsave(&device->rx_lock);
    if (!device->running || device->stopping) {
        spin_unlock_irqrestore(&device->rx_lock, rflags);
        return 0;
    }
    while (done < budget) {
        if (rtl8139_read8(device, RTL8139_REG_CR) & RTL8139_CR_BUFE) break;

        uint32_t           offset = device->cur_rx % RTL8139_RX_BUF_SIZE;
        volatile uint32_t *hdrp   = (volatile uint32_t *)(device->rx_ring + offset);
        uint32_t           hdr    = *hdrp;
        dma_read_barrier();

        uint16_t status = hdr & 0xffff;
        uint32_t length = hdr >> 16; // includes the 4-byte CRC
        int      good   = (status & RTL8139_RX_ROK) && !(status & RTL8139_RX_ERROR_MASK) && length > RTL8139_CRC_LEN
                   && length - RTL8139_CRC_LEN <= RTL8139_MAX_FRAME_SIZE;
        size_t frame_length = good ? length - RTL8139_CRC_LEN : 0;

        if (!good) {
            device->stats.rx_errors++;
            static uint64_t last_log;
            if (sched_ticks() - last_log >= 1000) {
                plogk("rtl8139: %s: RX error (status=%#x, length=%u)\n", device->netdev.name, (unsigned)status, (unsigned)length);
                last_log = sched_ticks();
            }
        } else if (offset + length > RTL8139_RX_BUF_SIZE) {
            uint32_t first = RTL8139_RX_BUF_SIZE - (offset + RTL8139_CRC_LEN);
            memcpy(frame, (const void *)(device->rx_ring + offset + RTL8139_CRC_LEN), first);
            memcpy(frame + first, (const void *)device->rx_ring, frame_length - first);
        } else {
            memcpy(frame, (const void *)(device->rx_ring + offset + RTL8139_CRC_LEN), frame_length);
        }

        device->cur_rx = (device->cur_rx + length + RTL8139_CRC_LEN + 3) & ~3u;
        rtl8139_write16(device, RTL8139_REG_CAPR, (uint16_t)(device->cur_rx - 16));
        done++;

        if (good) {
            spin_unlock_irqrestore(&device->rx_lock, rflags);
            net_pbuf_t *packet = net_pbuf_from(frame, frame_length, NET_PBUF_HEADROOM);
            if (!packet) {
                static uint64_t last_log;
                if (sched_ticks() - last_log >= 1000) {
                    plogk("rtl8139: %s: RX frame allocation failed.\n", device->netdev.name);
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

static void rtl8139_process_work(rtl8139_device_t *device, uint32_t cause)
{
    if (cause & RTL8139_ISR_PUN) rtl8139_update_link(device);
    if (cause & RTL8139_ISR_RER) {
        device->stats.rx_errors++;
        plogk("rtl8139: %s: Receive error interrupt.\n", device->netdev.name);
    }
    if (cause & (RTL8139_ISR_RXOVW | RTL8139_ISR_FOVW)) {
        device->stats.rx_errors++;
        device->stats.rx_overruns++;
        plogk("rtl8139: %s: RX buffer/FIFO overflow.\n", device->netdev.name);
        /* Re-synchronize the ring with the chip's write pointer. */
        device->cur_rx = rtl8139_read16(device, RTL8139_REG_CBR) % RTL8139_RX_BUF_SIZE;
        rtl8139_write16(device, RTL8139_REG_CAPR, (uint16_t)(device->cur_rx - 16));
    }
    if (cause & RTL8139_ISR_TER) {
        device->stats.tx_errors++;
        plogk("rtl8139: %s: Transmit error interrupt.\n", device->netdev.name);
    }
    if ((cause & RTL8139_RX_INT_MASK) || rtl8139_rx_ready(device)) (void)rtl8139_poll(device, RTL8139_WORK_BUDGET);

    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    if ((cause & RTL8139_ISR_TOK) || rtl8139_tx_ready_locked(device)) rtl8139_tx_reclaim_locked(device, RTL8139_TX_RECLAIM_BUDGET);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
}

static void rtl8139_worker(void *arg)
{
    rtl8139_device_t *device = arg;

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
        rtl8139_process_work(device, cause);

        rflags = spin_lock_irqsave(&device->work_lock);
        if (!device->stopping && !device->work_pending && !rtl8139_rx_ready(device) && !rtl8139_tx_ready(device)) {
            rtl8139_write16(device, RTL8139_REG_IMR, RTL8139_INT_MASK);
            rtl8139_write_flush(device);
            uint16_t status = rtl8139_read16(device, RTL8139_REG_ISR) & RTL8139_INT_MASK;
            if (status) {
                rtl8139_write16(device, RTL8139_REG_IMR, 0);
                rtl8139_write16(device, RTL8139_REG_ISR, status);
                device->work_pending |= status;
            }
        } else if (!device->stopping && !device->work_pending) {
            device->work_pending = RTL8139_ISR_ROK | RTL8139_ISR_TOK;
        }
        int more = !device->stopping && device->work_pending;
        spin_unlock_irqrestore(&device->work_lock, rflags);
        if (more) sched_yield();
    }
}

static int rtl8139_start_worker(rtl8139_device_t *device)
{
    if (device->worker_started) return 0;

    uint64_t rflags        = spin_lock_irqsave(&device->work_lock);
    device->worker_started = 1;
    device->worker_exited  = 0;
    device->work_pending   = RTL8139_WORK_INITIAL;
    spin_unlock_irqrestore(&device->work_lock, rflags);

    task_t *worker = kthread_create("rtl8139-rx", rtl8139_worker, device);
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

static void rtl8139_interrupt_device(rtl8139_device_t *device)
{
    uint16_t status = rtl8139_read16(device, RTL8139_REG_ISR);
    if (!(status & RTL8139_INT_MASK)) return;

    rtl8139_write16(device, RTL8139_REG_IMR, 0);
    rtl8139_write16(device, RTL8139_REG_ISR, status);
    rtl8139_write_flush(device);
    uint64_t rflags = spin_lock_irqsave(&device->work_lock);
    if (device->running && device->worker_started && !device->stopping) {
        device->work_pending |= status & RTL8139_INT_MASK;
        device->interrupts_pending++;
        wait_queue_wake_one(&device->work_wait);
    }
    spin_unlock_irqrestore(&device->work_lock, rflags);
}

static void rtl8139_interrupt_slot(size_t slot, void *frame)
{
    (void)frame;
    uint64_t          rflags = spin_lock_irqsave(&rtl8139_irq_lock);
    rtl8139_device_t *device = rtl8139_irq_slots[slot];
    if (device && !device->stopping)
        device->irq_active++;
    else
        device = NULL;
    spin_unlock_irqrestore(&rtl8139_irq_lock, rflags);

    if (device) {
        rtl8139_interrupt_device(device);
        rflags = spin_lock_irqsave(&rtl8139_irq_lock);
        device->irq_active--;
        spin_unlock_irqrestore(&rtl8139_irq_lock, rflags);
    }
    send_eoi();
}

#define RTL8139_IRQ_WRAPPERS(n)                                                     \
    static void rtl8139_legacy_interrupt_##n(void *frame)                           \
    {                                                                               \
        rtl8139_interrupt_slot(n, frame);                                           \
    }                                                                               \
    INTERRUPT_BEGIN static void rtl8139_idt_interrupt_##n(interrupt_frame_t *frame) \
    {                                                                               \
        rtl8139_interrupt_slot(n, frame);                                           \
    }                                                                               \
    INTERRUPT_END

RTL8139_IRQ_WRAPPERS(0)
RTL8139_IRQ_WRAPPERS(1)
RTL8139_IRQ_WRAPPERS(2)
RTL8139_IRQ_WRAPPERS(3)
RTL8139_IRQ_WRAPPERS(4)
RTL8139_IRQ_WRAPPERS(5)
RTL8139_IRQ_WRAPPERS(6)
RTL8139_IRQ_WRAPPERS(7)

static const net_irq_handler_fn rtl8139_legacy_irq_handlers[RTL8139_MAX_DEVICES] = {
    rtl8139_legacy_interrupt_0, rtl8139_legacy_interrupt_1, rtl8139_legacy_interrupt_2, rtl8139_legacy_interrupt_3,
    rtl8139_legacy_interrupt_4, rtl8139_legacy_interrupt_5, rtl8139_legacy_interrupt_6, rtl8139_legacy_interrupt_7,
};

static void *const rtl8139_idt_irq_handlers[RTL8139_MAX_DEVICES] = {
    (void *)rtl8139_idt_interrupt_0, (void *)rtl8139_idt_interrupt_1, (void *)rtl8139_idt_interrupt_2, (void *)rtl8139_idt_interrupt_3,
    (void *)rtl8139_idt_interrupt_4, (void *)rtl8139_idt_interrupt_5, (void *)rtl8139_idt_interrupt_6, (void *)rtl8139_idt_interrupt_7,
};

static int rtl8139_setup_interrupt(rtl8139_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&rtl8139_irq_lock);
    size_t   slot;
    for (slot = 0; slot < RTL8139_MAX_DEVICES; slot++)
        if (!rtl8139_irq_slots[slot]) break;
    if (slot == RTL8139_MAX_DEVICES) {
        spin_unlock_irqrestore(&rtl8139_irq_lock, rflags);
        plogk("rtl8139: %04x:%04x: No free IRQ slot.\n", (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
        return -ENOSPC;
    }
    device->irq_slot        = (uint8_t)slot;
    rtl8139_irq_slots[slot] = device;
    spin_unlock_irqrestore(&rtl8139_irq_lock, rflags);

    /*
     * The RTL8139 is a PCI 2.x-era chip whose config space generally lacks an
     * MSI capability, so MSI is not attempted here and legacy INTx is used.
     */
    device->irq = (uint8_t)pci_get_irq(device->pci);
    if (device->irq == 0 || device->irq == 0xff) goto fail;
    if (net_irq_claim_legacy && net_irq_release_legacy) {
        if (net_irq_claim_legacy(device->irq, rtl8139_legacy_irq_handlers[slot])) goto fail;
    } else {
        /*
         * Some platforms expose only INTx and this kernel may be built
         * without a shared legacy-IRQ dispatcher. Install an exclusive
         * fallback route so the device is not rejected before its RX
         * worker can start.
         */
        device->vector = IRQ_0 + device->irq;
        register_interrupt_handler((uint16_t)device->vector, rtl8139_idt_irq_handlers[slot], 0, 0x8e);
        ioapic_routing_t routing = {(uint8_t)device->vector, device->irq};
        ioapic_add(&routing);
        device->using_direct_legacy = 1;
    }
    device->using_legacy = 1;
    return 0;

fail:
    rflags                  = spin_lock_irqsave(&rtl8139_irq_lock);
    rtl8139_irq_slots[slot] = NULL;
    spin_unlock_irqrestore(&rtl8139_irq_lock, rflags);
    plogk("rtl8139: %04x:%04x: Interrupt setup failed.\n", (unsigned)device->pci->vendor_id, (unsigned)device->pci->device_id);
    return -ENODEV;
}

static void rtl8139_release_interrupt(rtl8139_device_t *device)
{
    if (!device->using_legacy) return;
    if (device->ioaddr) {
        rtl8139_write16(device, RTL8139_REG_IMR, 0);
        rtl8139_write16(device, RTL8139_REG_ISR, 0xffff);
        rtl8139_write_flush(device);
    }
    uint64_t rflags                     = spin_lock_irqsave(&rtl8139_irq_lock);
    rtl8139_irq_slots[device->irq_slot] = NULL;
    spin_unlock_irqrestore(&rtl8139_irq_lock, rflags);

    if (device->using_legacy && !device->using_direct_legacy && net_irq_release_legacy)
        net_irq_release_legacy(device->irq, rtl8139_legacy_irq_handlers[device->irq_slot]);
    for (;;) {
        rflags     = spin_lock_irqsave(&rtl8139_irq_lock);
        int active = device->irq_active != 0;
        spin_unlock_irqrestore(&rtl8139_irq_lock, rflags);
        if (!active) break;
        __asm__ volatile("pause" ::: "memory");
    }
    device->using_legacy = device->using_direct_legacy = 0;
}

static int rtl8139_netdev_name(char *name, size_t size)
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

static void rtl8139_destroy(rtl8139_device_t *device)
{
    if (!device) return;
    uint64_t rflags  = spin_lock_irqsave(&device->work_lock);
    device->stopping = 1;
    device->running  = 0;
    spin_unlock_irqrestore(&device->work_lock, rflags);
    rtl8139_release_interrupt(device);
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
    if (device->ioaddr) {
        rtl8139_write16(device, RTL8139_REG_IMR, 0);
        rtl8139_write8(device, RTL8139_REG_CR, 0);
        rtl8139_write16(device, RTL8139_REG_ISR, 0xffff);
        rtl8139_write_flush(device);
        msleep(10);
    }
    if (device->pci) {
        uint16_t command = pci_read_command_status(device->pci) & 0xffff;
        pci_write_command_status(device->pci, command & ~(1u << 2));
        (void)pci_read_command_status(device->pci);
    }
    dma_full_barrier();
    rtl8139_free_dma(device);
    if (device->pci) pci_write_command_status(device->pci, device->saved_command);
    free(device);
}

int rtl8139_probe(pci_device_cache_t *pci)
{
    if (!pci || rtl8139_device_count >= RTL8139_MAX_DEVICES) return -ENOSPC;
    const rtl8139_id_t *id = rtl8139_match((uint16_t)pci->vendor_id, (uint16_t)pci->device_id);
    if (!id) return -ENODEV;
    for (rtl8139_device_t *it = rtl8139_devices; it; it = it->next)
        if (it->pci == pci) return -EEXIST;

    rtl8139_device_t *device = malloc(sizeof(*device));
    if (!device) {
        plogk("rtl8139: %04x:%04x: Device allocation failed.\n", (unsigned)pci->vendor_id, (unsigned)pci->device_id);
        return -ENOMEM;
    }
    memset(device, 0, sizeof(*device));
    device->pci           = pci;
    device->device_id     = id->device;
    device->vector        = -1;
    device->saved_command = pci_read_command_status(pci) & 0xffff;
    wait_queue_init(&device->work_wait);
    wait_queue_init(&device->exit_wait);
    const char *stage = "I/O port mapping";

    /* BAR sizing writes all ones, so memory and I/O decoding must be off. */
    pci_write_command_status(pci, device->saved_command & ~((1u << 1) | (1u << 2)));
    int ret = rtl8139_get_ioaddr(device);
    if (ret) goto fail;
    pci_write_command_status(pci, device->saved_command | (1u << 1) | (1u << 2));
    stage = "reset";
    ret   = rtl8139_reset(device);
    if (ret) goto fail;
    rtl8139_read_mac(device);
    stage = "DMA buffers";
    ret   = rtl8139_alloc_dma(device);
    if (ret) goto fail;

    rtl8139_program_hw(device);
    stage = "interrupt setup";
    ret   = rtl8139_setup_interrupt(device);
    if (ret) goto fail;

    char netdev_name[NETDEV_NAME_MAX];
    stage = "netdev initialization";
    ret   = rtl8139_netdev_name(netdev_name, sizeof(netdev_name));
    if (ret) goto fail;
    ret = netdev_init(&device->netdev, netdev_name, &rtl8139_netdev_ops, device);
    if (ret) goto fail;
    memcpy(device->netdev.address, device->mac, sizeof(device->mac));
    device->netdev.mtu   = RTL8139_MTU;
    device->netdev.flags = NETDEV_F_BROADCAST;
    stage                = "netdev registration";
    ret                  = netdev_register(&device->netdev);
    if (ret) goto fail;
    device->netdev_registered = 1;
    device->next              = rtl8139_devices;
    rtl8139_devices           = device;
    device->running           = 1;
    rtl8139_device_count++;
    device->link_up = !!(rtl8139_read16(device, RTL8139_REG_BMSR) & RTL8139_BMSR_LINK);
    stage           = "netdev activation";
    ret             = netdev_set_up(&device->netdev, 1);
    if (ret) goto fail_linked;
    if (!device->link_up) {
        spin_lock(&device->netdev.lock);
        device->netdev.flags &= ~NETDEV_F_RUNNING;
        spin_unlock(&device->netdev.lock);
    }
    (void)rtl8139_read16(device, RTL8139_REG_ISR);
    if (rtl8139_scheduler_ready) {
        stage = "worker startup";
        ret   = rtl8139_start_worker(device);
        if (ret) goto fail_linked;
    }
    plogk("rtl8139: %s: Registered (MAC %02x:%02x:%02x:%02x:%02x:%02x, INTx, link %s)\n", device->netdev.name, device->mac[0], device->mac[1],
          device->mac[2], device->mac[3], device->mac[4], device->mac[5], device->link_up ? "up" : "down");
    return 0;

fail_linked:
    if (rtl8139_devices == device) rtl8139_devices = device->next;
    if (rtl8139_device_count) rtl8139_device_count--;

fail:
    plogk("rtl8139: Probe failed during %s (%d)\n", stage, ret);
    rtl8139_destroy(device);
    return ret;
}

int rtl8139_init(void)
{
#if !CONFIG_RTL8139
    return 0;
#endif
    int                  found = 0;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return -ENODEV;
    for (pci_device_cache_t *pci = cache->head; pci; pci = pci->next) {
        if (!rtl8139_match((uint16_t)pci->vendor_id, (uint16_t)pci->device_id)) continue;
        if (!rtl8139_probe(pci)) found++;
    }
    return found ? found : -ENODEV;
}

int rtl8139_start_workers(void)
{
#if !CONFIG_RTL8139
    return 0;
#endif
    int started = 0;
    int failed  = 0;

    rtl8139_scheduler_ready = 1;
    rtl8139_device_t **link = &rtl8139_devices;
    while (*link) {
        rtl8139_device_t *device = *link;
        if (device->worker_started || !rtl8139_start_worker(device)) {
            started++;
            link = &device->next;
            continue;
        }
        failed = 1;
        plogk("rtl8139: %s: Worker startup failed.\n", device->netdev.name);
        *link = device->next;
        rtl8139_device_count--;
        rtl8139_destroy(device);
    }
    return started ? started : (failed ? -ENOMEM : -ENODEV);
}

void rtl8139_shutdown(void)
{
    while (rtl8139_devices) {
        rtl8139_device_t *device = rtl8139_devices;
        rtl8139_devices          = device->next;
        rtl8139_device_count--;
        rtl8139_destroy(device);
    }
}

int rtl8139_link_up(const rtl8139_device_t *device)
{
    return device && device->link_up;
}

const uint8_t *rtl8139_mac_address(const rtl8139_device_t *device)
{
    return device ? device->mac : NULL;
}

const rtl8139_stats_t *rtl8139_get_stats(const rtl8139_device_t *device)
{
    return device ? &device->stats : NULL;
}

rtl8139_device_t *rtl8139_first_device(void)
{
    return rtl8139_devices;
}

rtl8139_device_t *rtl8139_next_device(rtl8139_device_t *device)
{
    return device ? device->next : NULL;
}
