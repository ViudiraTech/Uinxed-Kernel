/*
 *
 *      e1000.c
 *      Intel e1000/e1000e network controller driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/idt.h>
#include <chipset/common.h>
#include <drivers/interrupt/apic.h>
#include <drivers/net/e1000.h>
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

#define E1000_MAX_DEVICES       8
#define E1000_RX_COUNT          256
#define E1000_TX_COUNT          256
#define E1000_BUFFER_SIZE       2048
#define E1000_MAX_FRAME_SIZE    (E1000_MTU + 18)
#define E1000_WORK_BUDGET       64
#define E1000_TX_RECLAIM_BUDGET 64
#define E1000_RESET_TIMEOUT_US  100000
#define E1000_EEPROM_TIMEOUT_US 10000

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

#define E1000_ICR_TXDW     (1u << 0)
#define E1000_ICR_LSC      (1u << 2)
#define E1000_ICR_RXSEQ    (1u << 3)
#define E1000_ICR_RXDMT0   (1u << 4)
#define E1000_ICR_RXO      (1u << 6)
#define E1000_ICR_RXT0     (1u << 7)
#define E1000_INT_MASK     (E1000_ICR_TXDW | E1000_ICR_LSC | E1000_ICR_RXSEQ | E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0)
#define E1000_RX_INT_MASK  (E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0)
#define E1000_WORK_INITIAL (E1000_ICR_TXDW | E1000_ICR_LSC | E1000_ICR_RXT0)

#define E1000_RXD_STAT_DD  (1u << 0)
#define E1000_RXD_STAT_EOP (1u << 1)
#define E1000_TXD_STAT_DD  (1u << 0)
#define E1000_TXD_STAT_EC  (1u << 1)
#define E1000_TXD_STAT_LC  (1u << 2)
#define E1000_TXD_STAT_TU  (1u << 3)
#define E1000_TXD_ERROR    (E1000_TXD_STAT_EC | E1000_TXD_STAT_LC | E1000_TXD_STAT_TU)
#define E1000_TXD_CMD_EOP  (1u << 0)
#define E1000_TXD_CMD_IFCS (1u << 1)
#define E1000_TXD_CMD_RS   (1u << 3)

#define E1000_F_EERD_SMALL (1u << 0)
#define E1000_F_E1000E     (1u << 1)

typedef struct {
        uint16_t device;
        uint16_t flags;
} e1000_id_t;

/* These IDs use the legacy RX/TX descriptor and register layout implemented here. */
static const e1000_id_t e1000_ids[] = {
    {0x100e, 0                                  }, /* 82540EM, QEMU e1000 */
    {0x100f, 0                                  }, /* 82545EM */
    {0x1010, 0                                  }, /* 82546EB */
    {0x107c, E1000_F_EERD_SMALL                 }, /* 82541PI */
    {0x10d3, E1000_F_EERD_SMALL | E1000_F_E1000E}, /* 82574L, QEMU e1000e */
};

typedef struct {
        uint64_t address;
        uint16_t length;
        uint16_t checksum;
        uint8_t  status;
        uint8_t  errors;
        uint16_t special;
} __attribute__((packed, aligned(16))) e1000_rx_desc_t;

typedef struct {
        uint64_t address;
        uint16_t length;
        uint8_t  cso;
        uint8_t  command;
        uint8_t  status;
        uint8_t  css;
        uint16_t special;
} __attribute__((packed, aligned(16))) e1000_tx_desc_t;

typedef struct e1000_device {
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
        int                       owns_hw;
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
        volatile e1000_rx_desc_t *rx_ring;
        volatile e1000_tx_desc_t *tx_ring;
        uint64_t                  rx_ring_phys;
        uint64_t                  tx_ring_phys;
        uint64_t                  rx_buffer_phys[E1000_RX_COUNT];
        uint64_t                  tx_buffer_phys[E1000_TX_COUNT];
        uint16_t                  rx_next;
        uint16_t                  tx_next;
        uint16_t                  tx_clean;
        uint16_t                  tx_used;
        int                       rx_dropping;
        spinlock_t                rx_lock;
        spinlock_t                tx_lock;
        e1000_stats_t             stats;
        net_device_t              netdev;
        int                       netdev_registered;
        struct e1000_device      *next;
} e1000_device_t;

static e1000_device_t *e1000_devices;
static size_t          e1000_device_count;
static e1000_device_t *e1000_irq_slots[E1000_MAX_DEVICES];
static spinlock_t      e1000_irq_lock;
static int             e1000_scheduler_ready;

static inline uint32_t e1000_read(const e1000_device_t *device, uint32_t reg)
{
    return *(volatile uint32_t *)(device->mmio + reg);
}

static inline void e1000_write(e1000_device_t *device, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(device->mmio + reg) = value;
}

static inline void e1000_write_flush(e1000_device_t *device)
{
    (void)e1000_read(device, E1000_REG_STATUS);
}

static const e1000_id_t *e1000_match(uint16_t vendor, uint16_t device)
{
    if (vendor != E1000_VENDOR_INTEL) return NULL;
    for (size_t i = 0; i < sizeof(e1000_ids) / sizeof(e1000_ids[0]); i++)
        if (e1000_ids[i].device == device) return &e1000_ids[i];
    return NULL;
}

static int e1000_valid_mac(const uint8_t mac[6])
{
    uint8_t any = 0;
    uint8_t all = 0xff;
    for (size_t i = 0; i < 6; i++) {
        any |= mac[i];
        all &= mac[i];
    }
    return any != 0 && all != 0xff && !(mac[0] & 1);
}

static int e1000_eeprom_read(e1000_device_t *device, uint8_t word, uint16_t *value)
{
    uint32_t done       = (device->features & E1000_F_EERD_SMALL) ? (1u << 1) : (1u << 4);
    uint32_t addr_shift = (device->features & E1000_F_EERD_SMALL) ? 2 : 8;

    e1000_write(device, E1000_REG_EERD, 1u | ((uint32_t)word << addr_shift));
    for (uint32_t i = 0; i < E1000_EEPROM_TIMEOUT_US; i++) {
        uint32_t eerd = e1000_read(device, E1000_REG_EERD);
        if (eerd & done) {
            *value = (uint16_t)(eerd >> 16);
            return 0;
        }
        usleep(1);
    }
    return -ETIMEDOUT;
}

static int e1000_read_mac(e1000_device_t *device)
{
    uint16_t words[3];
    int      ok = 1;

    for (uint8_t i = 0; i < 3; i++) {
        if (e1000_eeprom_read(device, i, &words[i])) {
            ok = 0;
            break;
        }
        size_t byte           = (size_t)i * 2;
        device->mac[byte]     = words[i] & 0xff;
        device->mac[byte + 1] = words[i] >> 8;
    }
    if (ok && e1000_valid_mac(device->mac)) return 0;

    uint32_t ral = e1000_read(device, E1000_REG_RAL0);
    uint32_t rah = e1000_read(device, E1000_REG_RAH0);
    if (!(rah & E1000_RAH_AV)) return -ENODEV;
    device->mac[0] = ral;
    device->mac[1] = ral >> 8;
    device->mac[2] = ral >> 16;
    device->mac[3] = ral >> 24;
    device->mac[4] = rah;
    device->mac[5] = rah >> 8;
    return e1000_valid_mac(device->mac) ? 0 : -ENODEV;
}

static int e1000_map_bar(e1000_device_t *device)
{
    base_address_register_t bar = get_base_address_register(device->pci, 0);
    uint32_t                raw = read_bar_n(device->pci, 0);
    uint64_t                phys;

    if (!bar.address || bar.type != mem_mapping || raw == 0xffffffff || (raw & 1) || (((raw >> 1) & 3) == BAR_Reserved)) return -ENODEV;
    phys = raw & ~0xfull;
    if (((raw >> 1) & 3) == BAR_S64) {
        uint32_t high = read_bar_n(device->pci, 1);
        if (high == 0xffffffff) return -ENODEV;
        phys |= (uint64_t)high << 32;
    }
    if (!phys) return -ENODEV;

    device->mmio_size = bar.size & ~BAR_64BIT_FLAG;
    if (device->mmio_size < E1000_REG_RAH0 + sizeof(uint32_t)) return -ENODEV;
    if (phys + device->mmio_size < phys) return -EINVAL;
    uint64_t start = phys & ~(PAGE_4K_SIZE - 1);
    uint64_t end   = (phys + device->mmio_size + PAGE_4K_SIZE - 1) & ~(PAGE_4K_SIZE - 1);
    page_map_range_to(get_kernel_pagedir(), start, end - start, PTE_MMIO_FLAGS);
    device->mmio_phys = phys;
    device->mmio      = (volatile uint8_t *)phys_to_virt(phys);
    return 0;
}

static int e1000_reset(e1000_device_t *device)
{
    e1000_write(device, E1000_REG_IMC, 0xffffffff);
    e1000_write(device, E1000_REG_RCTL, 0);
    e1000_write(device, E1000_REG_TCTL, 0);
    e1000_write_flush(device);
    msleep(10);

    e1000_write(device, E1000_REG_CTRL, e1000_read(device, E1000_REG_CTRL) | E1000_CTRL_RST);
    e1000_write_flush(device);
    for (uint32_t i = 0; i < E1000_RESET_TIMEOUT_US; i++) {
        if (!(e1000_read(device, E1000_REG_CTRL) & E1000_CTRL_RST)) {
            msleep(10);
            e1000_write(device, E1000_REG_IMC, 0xffffffff);
            (void)e1000_read(device, E1000_REG_ICR);
            return 0;
        }
        usleep(1);
    }
    return -ETIMEDOUT;
}

static void e1000_free_dma(e1000_device_t *device)
{
    for (size_t i = 0; i < E1000_RX_COUNT; i++) {
        if (device->rx_buffer_phys[i]) free_frames(device->rx_buffer_phys[i], 1);
        device->rx_buffer_phys[i] = 0;
    }
    for (size_t i = 0; i < E1000_TX_COUNT; i++) {
        if (device->tx_buffer_phys[i]) free_frames(device->tx_buffer_phys[i], 1);
        device->tx_buffer_phys[i] = 0;
    }
    if (device->rx_ring_phys) free_frames(device->rx_ring_phys, 1);
    if (device->tx_ring_phys) free_frames(device->tx_ring_phys, 1);
    device->rx_ring_phys = device->tx_ring_phys = 0;
    device->rx_ring                             = NULL;
    device->tx_ring                             = NULL;
}

static int e1000_alloc_dma(e1000_device_t *device)
{
    device->rx_ring_phys = alloc_frames(1);
    if (!device->rx_ring_phys) return -ENOMEM;
    device->tx_ring_phys = alloc_frames(1);
    if (!device->tx_ring_phys) return -ENOMEM;
    device->rx_ring = (volatile e1000_rx_desc_t *)phys_to_virt(device->rx_ring_phys);
    device->tx_ring = (volatile e1000_tx_desc_t *)phys_to_virt(device->tx_ring_phys);
    memset((void *)device->rx_ring, 0, PAGE_4K_SIZE);
    memset((void *)device->tx_ring, 0, PAGE_4K_SIZE);

    for (size_t i = 0; i < E1000_RX_COUNT; i++) {
        device->rx_buffer_phys[i] = alloc_frames(1);
        if (!device->rx_buffer_phys[i]) return -ENOMEM;
        device->rx_ring[i].address = device->rx_buffer_phys[i];
    }
    for (size_t i = 0; i < E1000_TX_COUNT; i++) {
        device->tx_buffer_phys[i] = alloc_frames(1);
        if (!device->tx_buffer_phys[i]) return -ENOMEM;
        device->tx_ring[i].address = device->tx_buffer_phys[i];
        device->tx_ring[i].status  = E1000_TXD_STAT_DD;
    }
    dma_write_barrier();
    return 0;
}

static void e1000_program_mac(e1000_device_t *device)
{
    uint32_t ral
        = (uint32_t)device->mac[0] | ((uint32_t)device->mac[1] << 8) | ((uint32_t)device->mac[2] << 16) | ((uint32_t)device->mac[3] << 24);
    uint32_t rah = (uint32_t)device->mac[4] | ((uint32_t)device->mac[5] << 8) | E1000_RAH_AV;
    e1000_write(device, E1000_REG_RAL0, ral);
    e1000_write(device, E1000_REG_RAH0, rah);
    for (size_t i = 0; i < 128; i++) e1000_write(device, E1000_REG_MTA + (uint32_t)i * 4, 0);
}

static void e1000_program_rings(e1000_device_t *device)
{
    e1000_write(device, E1000_REG_RDBAL, (uint32_t)device->rx_ring_phys);
    e1000_write(device, E1000_REG_RDBAH, (uint32_t)(device->rx_ring_phys >> 32));
    e1000_write(device, E1000_REG_RDLEN, E1000_RX_COUNT * sizeof(e1000_rx_desc_t));
    e1000_write(device, E1000_REG_RDH, 0);
    e1000_write(device, E1000_REG_RDT, E1000_RX_COUNT - 1);
    e1000_write(device, E1000_REG_RDTR, 0);
    e1000_write(device, E1000_REG_RADV, 0);

    e1000_write(device, E1000_REG_TDBAL, (uint32_t)device->tx_ring_phys);
    e1000_write(device, E1000_REG_TDBAH, (uint32_t)(device->tx_ring_phys >> 32));
    e1000_write(device, E1000_REG_TDLEN, E1000_TX_COUNT * sizeof(e1000_tx_desc_t));
    e1000_write(device, E1000_REG_TDH, 0);
    e1000_write(device, E1000_REG_TDT, 0);
    e1000_write(device, E1000_REG_TIDV, 0);
    e1000_write(device, E1000_REG_TADV, 0);
    dma_write_barrier();

    e1000_write(device, E1000_REG_TIPG, 8 | (8u << 10) | (6u << 20));
    e1000_write(device, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x0fu << E1000_TCTL_CT_SHIFT) | (0x40u << E1000_TCTL_COLD_SHIFT));
    e1000_write(device, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_write_flush(device);
}

static void e1000_update_link(e1000_device_t *device)
{
    int up = !!(e1000_read(device, E1000_REG_STATUS) & E1000_STATUS_LU);
    if (up == device->link_up) return;
    device->link_up = up;
    device->stats.link_changes++;
    if (!up) plogk("e1000: %s: link down.\n", device->netdev.name);
    if (device->netdev_registered) {
        spin_lock(&device->netdev.lock);
        if (up && (device->netdev.flags & NETDEV_F_UP))
            device->netdev.flags |= NETDEV_F_RUNNING;
        else
            device->netdev.flags &= ~NETDEV_F_RUNNING;
        spin_unlock(&device->netdev.lock);
    }
}

static size_t e1000_tx_reclaim_locked(e1000_device_t *device, size_t budget)
{
    size_t reclaimed = 0;

    while (device->tx_used && reclaimed < budget) {
        volatile e1000_tx_desc_t *desc = &device->tx_ring[device->tx_clean];
        if (!(desc->status & E1000_TXD_STAT_DD)) break;
        dma_read_barrier();
        if (desc->status & E1000_TXD_ERROR) {
            device->stats.tx_errors++;
            plogk("e1000: %s: TX descriptor error (status=%#x).\n", device->netdev.name, (unsigned)desc->status);
        } else {
            device->stats.tx_packets++;
            device->stats.tx_bytes += desc->length;
        }
        device->tx_clean = (device->tx_clean + 1) % E1000_TX_COUNT;
        device->tx_used--;
        reclaimed++;
    }
    return reclaimed;
}

static int e1000_rx_ready_locked(e1000_device_t *device)
{
    dma_read_barrier();
    return !!(device->rx_ring[device->rx_next].status & E1000_RXD_STAT_DD);
}

static int e1000_rx_ready(e1000_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&device->rx_lock);
    int      ready  = e1000_rx_ready_locked(device);
    spin_unlock_irqrestore(&device->rx_lock, rflags);
    return ready;
}

static int e1000_tx_ready_locked(e1000_device_t *device)
{
    dma_read_barrier();
    return device->tx_used && !!(device->tx_ring[device->tx_clean].status & E1000_TXD_STAT_DD);
}

static int e1000_tx_ready(e1000_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    int      ready  = e1000_tx_ready_locked(device);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
    return ready;
}

static int e1000_net_open(net_device_t *netdev)
{
    e1000_device_t *device = netdev_private(netdev);
    if (!device || !device->running) return -ENODEV;
    return 0;
}

static void e1000_net_stop(net_device_t *netdev)
{
    (void)netdev;
}

static int e1000_net_xmit(net_device_t *netdev, net_pbuf_t *packet)
{
    e1000_device_t *device = netdev_private(netdev);
    if (!packet) return -EINVAL;
    return e1000_transmit(device, packet->data, packet->length);
}

static int e1000_net_set_mtu(net_device_t *netdev, uint32_t mtu)
{
    (void)netdev;
    return mtu == E1000_MTU ? 0 : -EOPNOTSUPP;
}

static const netdev_ops_t e1000_netdev_ops = {
    .open    = e1000_net_open,
    .stop    = e1000_net_stop,
    .xmit    = e1000_net_xmit,
    .set_mtu = e1000_net_set_mtu,
};

int e1000_transmit(e1000_device_t *device, const void *packet, size_t length)
{
    if (!device || !packet || length == 0) return -EINVAL;
    if (length > E1000_MAX_FRAME_SIZE) {
        device->stats.tx_dropped++;
        device->stats.tx_errors++;
        return -EMSGSIZE;
    }
    if (!device->running) return -ENODEV;
    if (!device->link_up) return -ENETDOWN;

    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    e1000_tx_reclaim_locked(device, E1000_TX_RECLAIM_BUDGET);
    if (!device->running || device->stopping) {
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -ENODEV;
    }
    if (!device->link_up) {
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -ENETDOWN;
    }
    /* Keep one descriptor unused so equal head and tail always means empty. */
    if (device->tx_used == E1000_TX_COUNT - 1) {
        device->stats.tx_busy++;
        spin_unlock_irqrestore(&device->tx_lock, rflags);
        return -EAGAIN;
    }

    uint16_t idx = device->tx_next;
    memcpy(phys_to_virt(device->tx_buffer_phys[idx]), packet, length);
    device->tx_ring[idx].length  = (uint16_t)length;
    device->tx_ring[idx].cso     = 0;
    device->tx_ring[idx].command = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    device->tx_ring[idx].css     = 0;
    device->tx_ring[idx].special = 0;
    device->tx_ring[idx].status  = 0;
    device->tx_next              = (idx + 1) % E1000_TX_COUNT;
    device->tx_used++;
    dma_write_barrier();
    e1000_write(device, E1000_REG_TDT, device->tx_next);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
    return 0;
}

size_t e1000_poll(e1000_device_t *device, size_t budget)
{
    size_t  done = 0;
    uint8_t frame[E1000_MAX_FRAME_SIZE];
    if (!device || !device->running) return 0;

    uint64_t rflags = spin_lock_irqsave(&device->rx_lock);
    if (!device->running || device->stopping) {
        spin_unlock_irqrestore(&device->rx_lock, rflags);
        return 0;
    }
    while (done < budget) {
        uint16_t                  idx    = device->rx_next;
        volatile e1000_rx_desc_t *desc   = &device->rx_ring[idx];
        uint8_t                   status = desc->status;
        if (!(status & E1000_RXD_STAT_DD)) break;
        dma_read_barrier();

        size_t length       = desc->length;
        size_t frame_length = 0;
        if (desc->errors || !length || length > E1000_MAX_FRAME_SIZE || !(status & E1000_RXD_STAT_EOP)) {
            /* A standard frame fits one 2 KiB buffer; chained descriptors are jumbo input. */
            if (!device->rx_dropping) device->stats.rx_errors++;
            device->rx_dropping = 1;
        } else if (!device->rx_dropping) {
            memcpy(frame, phys_to_virt(device->rx_buffer_phys[idx]), length);
            frame_length = length;
        }

        if (status & E1000_RXD_STAT_EOP) {
            if (device->rx_dropping) device->stats.rx_dropped++;
            device->rx_dropping = 0;
        }

        desc->length   = 0;
        desc->checksum = 0;
        desc->errors   = 0;
        desc->special  = 0;
        dma_write_barrier();
        desc->status = 0;
        dma_write_barrier();
        device->rx_next = (idx + 1) % E1000_RX_COUNT;
        e1000_write(device, E1000_REG_RDT, idx);
        done++;

        if (frame_length) {
            spin_unlock_irqrestore(&device->rx_lock, rflags);
            net_pbuf_t *packet = net_pbuf_from(frame, frame_length, NET_PBUF_HEADROOM);
            if (!packet)
                device->stats.rx_dropped++;
            else {
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

static void e1000_process_work(e1000_device_t *device, uint32_t cause)
{
    if (cause & E1000_ICR_LSC) e1000_update_link(device);
    if (cause & E1000_ICR_RXO) {
        device->stats.rx_errors++;
        device->stats.rx_overruns++;
        plogk("e1000: %s: RX overrun.\n", device->netdev.name);
    }
    if (cause & E1000_ICR_RXSEQ) device->stats.rx_errors++;
    if ((cause & E1000_RX_INT_MASK) || e1000_rx_ready(device)) (void)e1000_poll(device, E1000_WORK_BUDGET);

    uint64_t rflags = spin_lock_irqsave(&device->tx_lock);
    if ((cause & E1000_ICR_TXDW) || e1000_tx_ready_locked(device)) e1000_tx_reclaim_locked(device, E1000_TX_RECLAIM_BUDGET);
    spin_unlock_irqrestore(&device->tx_lock, rflags);
}

static void e1000_worker(void *arg)
{
    e1000_device_t *device = arg;

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
        e1000_process_work(device, cause);

        rflags = spin_lock_irqsave(&device->work_lock);
        if (!device->stopping && !device->work_pending && !e1000_rx_ready(device) && !e1000_tx_ready(device)) {
            e1000_write(device, E1000_REG_IMS, E1000_INT_MASK);
            e1000_write_flush(device);
            cause = e1000_read(device, E1000_REG_ICR) & E1000_INT_MASK;
            if (cause) {
                e1000_write(device, E1000_REG_IMC, E1000_INT_MASK);
                e1000_write_flush(device);
                device->work_pending |= cause;
            }
        } else if (!device->stopping && !device->work_pending) {
            device->work_pending = E1000_ICR_RXT0 | E1000_ICR_TXDW;
        }
        int more = !device->stopping && device->work_pending;
        spin_unlock_irqrestore(&device->work_lock, rflags);
        if (more) sched_yield();
    }
}

static int e1000_start_worker(e1000_device_t *device)
{
    if (device->worker_started) return 0;

    uint64_t rflags        = spin_lock_irqsave(&device->work_lock);
    device->worker_started = 1;
    device->worker_exited  = 0;
    device->work_pending   = E1000_WORK_INITIAL;
    spin_unlock_irqrestore(&device->work_lock, rflags);

    task_t *worker = kthread_create("e1000-rx", e1000_worker, device);
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

static void e1000_interrupt_device(e1000_device_t *device)
{
    uint32_t cause = e1000_read(device, E1000_REG_ICR);
    if (!(cause & E1000_INT_MASK)) return;

    e1000_write(device, E1000_REG_IMC, E1000_INT_MASK);
    e1000_write_flush(device);
    uint64_t rflags = spin_lock_irqsave(&device->work_lock);
    if (device->running && device->worker_started && !device->stopping) {
        device->work_pending |= cause & E1000_INT_MASK;
        device->interrupts_pending++;
        wait_queue_wake_one(&device->work_wait);
    }
    spin_unlock_irqrestore(&device->work_lock, rflags);
}

static void e1000_interrupt_slot(size_t slot, void *frame)
{
    (void)frame;
    uint64_t        rflags = spin_lock_irqsave(&e1000_irq_lock);
    e1000_device_t *device = e1000_irq_slots[slot];
    if (device && !device->stopping)
        device->irq_active++;
    else
        device = NULL;
    spin_unlock_irqrestore(&e1000_irq_lock, rflags);

    if (device) {
        e1000_interrupt_device(device);
        rflags = spin_lock_irqsave(&e1000_irq_lock);
        device->irq_active--;
        spin_unlock_irqrestore(&e1000_irq_lock, rflags);
    }
    send_eoi();
}

#define E1000_IRQ_WRAPPERS(n)                                                     \
    static void e1000_legacy_interrupt_##n(void *frame)                           \
    {                                                                             \
        e1000_interrupt_slot(n, frame);                                           \
    }                                                                             \
    INTERRUPT_BEGIN static void e1000_idt_interrupt_##n(interrupt_frame_t *frame) \
    {                                                                             \
        e1000_interrupt_slot(n, frame);                                           \
    }                                                                             \
    INTERRUPT_END

E1000_IRQ_WRAPPERS(0)
E1000_IRQ_WRAPPERS(1)
E1000_IRQ_WRAPPERS(2)
E1000_IRQ_WRAPPERS(3)
E1000_IRQ_WRAPPERS(4)
E1000_IRQ_WRAPPERS(5)
E1000_IRQ_WRAPPERS(6)
E1000_IRQ_WRAPPERS(7)

static const net_irq_handler_fn e1000_legacy_irq_handlers[E1000_MAX_DEVICES] = {
    e1000_legacy_interrupt_0, e1000_legacy_interrupt_1, e1000_legacy_interrupt_2, e1000_legacy_interrupt_3,
    e1000_legacy_interrupt_4, e1000_legacy_interrupt_5, e1000_legacy_interrupt_6, e1000_legacy_interrupt_7,
};

static void *const e1000_idt_irq_handlers[E1000_MAX_DEVICES] = {
    (void *)e1000_idt_interrupt_0, (void *)e1000_idt_interrupt_1, (void *)e1000_idt_interrupt_2, (void *)e1000_idt_interrupt_3,
    (void *)e1000_idt_interrupt_4, (void *)e1000_idt_interrupt_5, (void *)e1000_idt_interrupt_6, (void *)e1000_idt_interrupt_7,
};

static int e1000_setup_interrupt(e1000_device_t *device)
{
    uint64_t rflags = spin_lock_irqsave(&e1000_irq_lock);
    size_t   slot;
    for (slot = 0; slot < E1000_MAX_DEVICES; slot++)
        if (!e1000_irq_slots[slot]) break;
    if (slot == E1000_MAX_DEVICES) {
        spin_unlock_irqrestore(&e1000_irq_lock, rflags);
        return -ENOSPC;
    }
    device->irq_slot      = (uint8_t)slot;
    e1000_irq_slots[slot] = device;
    spin_unlock_irqrestore(&e1000_irq_lock, rflags);

    pci_msi_init(device->pci);
    device->vector = pci_enable_msi(device->pci);
    if (device->vector >= 0) {
        register_interrupt_handler((uint16_t)device->vector, e1000_idt_irq_handlers[slot], 0, 0x8e);
        device->using_msi = 1;
        return 0;
    }

    device->irq = (uint8_t)pci_get_irq(device->pci);
    if (device->irq == 0 || device->irq == 0xff) goto fail;
    if (net_irq_claim_legacy && net_irq_release_legacy) {
        if (net_irq_claim_legacy(device->irq, e1000_legacy_irq_handlers[slot])) goto fail;
    } else {
        /*
         * Some platforms (including QEMU's 82540EM) expose only INTx and
         * this kernel may be built without a shared legacy-IRQ dispatcher.
         * Install an exclusive fallback route so the device is not rejected
         * before its RX worker can start.
        */
        device->vector = IRQ_0 + device->irq;
        register_interrupt_handler((uint16_t)device->vector, e1000_idt_irq_handlers[slot], 0, 0x8e);
        ioapic_routing_t routing = {(uint8_t)device->vector, device->irq};
        ioapic_add(&routing);
        device->using_direct_legacy = 1;
    }
    device->using_legacy = 1;
    return 0;

fail:
    rflags                = spin_lock_irqsave(&e1000_irq_lock);
    e1000_irq_slots[slot] = NULL;
    spin_unlock_irqrestore(&e1000_irq_lock, rflags);
    return -ENODEV;
}

static void e1000_release_interrupt(e1000_device_t *device)
{
    if (!device->using_msi && !device->using_legacy) return;
    if (device->mmio) {
        e1000_write(device, E1000_REG_IMC, 0xffffffff);
        e1000_write_flush(device);
    }
    uint64_t rflags                   = spin_lock_irqsave(&e1000_irq_lock);
    e1000_irq_slots[device->irq_slot] = NULL;
    spin_unlock_irqrestore(&e1000_irq_lock, rflags);

    if (device->using_msi) pci_disable_msi(device->pci);
    if (device->using_legacy && !device->using_direct_legacy && net_irq_release_legacy)
        net_irq_release_legacy(device->irq, e1000_legacy_irq_handlers[device->irq_slot]);
    for (;;) {
        rflags     = spin_lock_irqsave(&e1000_irq_lock);
        int active = device->irq_active != 0;
        spin_unlock_irqrestore(&e1000_irq_lock, rflags);
        if (!active) break;
        __asm__ volatile("pause" ::: "memory");
    }
    device->using_msi = device->using_legacy = device->using_direct_legacy = 0;
}

static void e1000_destroy(e1000_device_t *device)
{
    if (!device) return;
    uint64_t rflags  = spin_lock_irqsave(&device->work_lock);
    device->stopping = 1;
    device->running  = 0;
    spin_unlock_irqrestore(&device->work_lock, rflags);
    e1000_release_interrupt(device);
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
        e1000_write(device, E1000_REG_IMC, 0xffffffff);
        e1000_write(device, E1000_REG_RCTL, 0);
        e1000_write(device, E1000_REG_TCTL, 0);
        if (device->owns_hw) e1000_write(device, E1000_REG_CTRL_EXT, e1000_read(device, E1000_REG_CTRL_EXT) & ~E1000_CTRL_EXT_DRV_LOAD);
        e1000_write_flush(device);
        msleep(10);
    }
    if (device->pci) {
        uint16_t command = pci_read_command_status(device->pci) & 0xffff;
        pci_write_command_status(device->pci, command & ~(1u << 2));
        (void)pci_read_command_status(device->pci);
    }
    dma_full_barrier();
    e1000_free_dma(device);
    if (device->pci) pci_write_command_status(device->pci, device->saved_command);
    free(device);
}

int e1000_probe(pci_device_cache_t *pci)
{
    if (!pci || e1000_device_count >= E1000_MAX_DEVICES) return -ENOSPC;
    const e1000_id_t *id = e1000_match((uint16_t)pci->vendor_id, (uint16_t)pci->device_id);
    if (!id) return -ENODEV;
    for (e1000_device_t *it = e1000_devices; it; it = it->next)
        if (it->pci == pci) return -EEXIST;

    e1000_device_t *device = malloc(sizeof(*device));
    if (!device) return -ENOMEM;
    memset(device, 0, sizeof(*device));
    device->pci           = pci;
    device->device_id     = id->device;
    device->features      = id->flags;
    device->vector        = -1;
    device->saved_command = pci_read_command_status(pci) & 0xffff;
    wait_queue_init(&device->work_wait);
    wait_queue_init(&device->exit_wait);
    const char *stage = "BAR mapping";

    /* BAR sizing writes all ones, so memory decoding and DMA must be off. */
    pci_write_command_status(pci, device->saved_command & ~((1u << 1) | (1u << 2)));
    int ret = e1000_map_bar(device);
    if (ret) goto fail;
    pci_write_command_status(pci, device->saved_command | (1u << 1) | (1u << 2));
    if ((device->features & E1000_F_E1000E) && (e1000_read(device, E1000_REG_CTRL_EXT) & E1000_CTRL_EXT_DRV_LOAD)) {
        stage = "hardware ownership";
        ret   = -EBUSY;
        goto fail;
    }
    stage = "reset";
    ret   = e1000_reset(device);
    if (ret) goto fail;
    if (device->features & E1000_F_E1000E) {
        e1000_write(device, E1000_REG_CTRL_EXT, e1000_read(device, E1000_REG_CTRL_EXT) | E1000_CTRL_EXT_DRV_LOAD);
        e1000_write_flush(device);
        device->owns_hw = 1;
    }
    stage = "MAC address";
    ret   = e1000_read_mac(device);
    if (ret) goto fail;
    stage = "DMA rings";
    ret   = e1000_alloc_dma(device);
    if (ret) goto fail;

    e1000_program_mac(device);
    e1000_program_rings(device);
    e1000_write(device, E1000_REG_CTRL, e1000_read(device, E1000_REG_CTRL) | E1000_CTRL_SLU);
    e1000_write(device, E1000_REG_ITR, 8000);
    stage = "interrupt setup";
    ret   = e1000_setup_interrupt(device);
    if (ret) goto fail;

    char netdev_name[NETDEV_NAME_MAX];
    (void)snprintf(netdev_name, sizeof(netdev_name), "eth%u", (unsigned)e1000_device_count);
    stage = "netdev initialization";
    ret   = netdev_init(&device->netdev, netdev_name, &e1000_netdev_ops, device);
    if (ret) goto fail;
    memcpy(device->netdev.address, device->mac, sizeof(device->mac));
    device->netdev.mtu   = E1000_MTU;
    device->netdev.flags = NETDEV_F_BROADCAST;
    stage                = "netdev registration";
    ret                  = netdev_register(&device->netdev);
    if (ret) goto fail;
    device->netdev_registered = 1;
    device->next              = e1000_devices;
    e1000_devices             = device;
    device->running           = 1;
    e1000_device_count++;
    device->link_up = !!(e1000_read(device, E1000_REG_STATUS) & E1000_STATUS_LU);
    stage           = "netdev activation";
    ret             = netdev_set_up(&device->netdev, 1);
    if (ret) goto fail_linked;
    if (!device->link_up) {
        spin_lock(&device->netdev.lock);
        device->netdev.flags &= ~NETDEV_F_RUNNING;
        spin_unlock(&device->netdev.lock);
    }
    (void)e1000_read(device, E1000_REG_ICR);
    if (e1000_scheduler_ready) {
        stage = "worker startup";
        ret   = e1000_start_worker(device);
        if (ret) goto fail_linked;
    }
    return 0;

fail_linked:
    if (e1000_devices == device) e1000_devices = device->next;
    if (e1000_device_count) e1000_device_count--;

fail:
    plogk("e1000: Probe failed during %s (%d).\n", stage, ret);
    e1000_destroy(device);
    return ret;
}

int e1000_init(void)
{
#if !CONFIG_E1000
    return 0;
#endif
    int                  found = 0;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return -ENODEV;
    for (pci_device_cache_t *pci = cache->head; pci; pci = pci->next) {
        if (!e1000_match((uint16_t)pci->vendor_id, (uint16_t)pci->device_id)) continue;
        if (!e1000_probe(pci)) found++;
    }
    return found ? found : -ENODEV;
}

int e1000_start_workers(void)
{
#if !CONFIG_E1000
    return 0;
#endif
    int started = 0;
    int failed  = 0;

    e1000_scheduler_ready = 1;
    e1000_device_t **link = &e1000_devices;
    while (*link) {
        e1000_device_t *device = *link;
        if (device->worker_started || !e1000_start_worker(device)) {
            started++;
            link = &device->next;
            continue;
        }
        failed = 1;
        plogk("e1000: %s: worker startup failed.\n", device->netdev.name);
        *link = device->next;
        e1000_device_count--;
        e1000_destroy(device);
    }
    return started ? started : (failed ? -ENOMEM : -ENODEV);
}

void e1000_shutdown(void)
{
    while (e1000_devices) {
        e1000_device_t *device = e1000_devices;
        e1000_devices          = device->next;
        e1000_device_count--;
        e1000_destroy(device);
    }
}

int e1000_link_up(const e1000_device_t *device)
{
    return device && device->link_up;
}

const uint8_t *e1000_mac_address(const e1000_device_t *device)
{
    return device ? device->mac : NULL;
}

const e1000_stats_t *e1000_get_stats(const e1000_device_t *device)
{
    return device ? &device->stats : NULL;
}

e1000_device_t *e1000_first_device(void)
{
    return e1000_devices;
}

e1000_device_t *e1000_next_device(e1000_device_t *device)
{
    return device ? device->next : NULL;
}
