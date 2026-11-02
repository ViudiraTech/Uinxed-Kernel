/*
 *
 *      floppy.c
 *      PC floppy disk controller driver
 *
 *      2026/11/1 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/firmware/cmos.h>
#include <arch/firmware/dma.h>
#include <arch/misc/common.h>
#include <arch/tables/idt.h>
#include <drivers/block/core/blockdev.h>
#include <drivers/block/core/gendisk.h>
#include <drivers/block/floppy/floppy.h>
#include <drivers/firmware/apic.h>
#include <kernel/debug/printk.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <process/kthread.h>
#include <process/sched.h>
#include <sync/spin_lock.h>

#if CONFIG_FLOPPY

static const floppy_geometry_t floppy_geometries[] = {
    {FLOPPY_TYPE_NONE,       0,  0, 0,  0, 0   },
    {FLOPPY_TYPE_360K,       40, 2, 9,  1, 720 }, // 360 KiB
    {FLOPPY_TYPE_1200K,      80, 2, 15, 0, 2400}, // 1.2 MiB
    {FLOPPY_TYPE_720K,       80, 2, 9,  2, 1440}, // 720 KiB
    {FLOPPY_TYPE_1440K,      80, 2, 18, 0, 2880}, // 1.44 MiB
    {FLOPPY_TYPE_2880K_5_25, 80, 2, 36, 3, 5760}, // 2.88 MiB
    {FLOPPY_TYPE_2880K_3_5,  80, 2, 36, 3, 5760}, // 2.88 MiB
};

static const char *const floppy_type_names[] = {
    "none", "360 KiB", "1.2 MiB", "720 KiB", "1.44 MiB", "2.88 MiB 5.25\"", "2.88 MiB 3.5\"",
};

static floppy_drive_t    drives[FLOPPY_MAX_DRIVES];
static uint64_t          dma_buffer_phys;
static uint8_t          *dma_buffer;
static int               floppy_ops_id = -1;
static raw_spinlock_t    fdc_lock;
static volatile uint32_t fdc_irq_seen;
static bool              fdc_ready;
static uint8_t           fdc_motor_mask;
static wait_queue_t      floppy_spinup_wait;
static wait_queue_t      floppy_motor_wait;

/* Return the configured geometry for a detected drive. */
const floppy_geometry_t *floppy_get_geometry(uint8_t drive)
{
    if (drive >= FLOPPY_MAX_DRIVES || !drives[drive].present) return NULL;
    floppy_type_t type = drives[drive].type;

    if (!type || type >= sizeof(floppy_geometries) / sizeof(floppy_geometries[0])) return NULL;
    return &floppy_geometries[type];
}

/* Report whether CMOS describes a supported floppy drive. */
bool floppy_drive_present(uint8_t drive)
{
    return floppy_get_geometry(drive) != NULL;
}

/* IRQ 6 only records completion; command/result handling stays in process context. */
INTERRUPT_BEGIN static void floppy_irq(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    __atomic_store_n(&fdc_irq_seen, 1, __ATOMIC_RELEASE);
    send_eoi();
    irq_leave_gs_no_preempt(frame);
}
INTERRUPT_END

/* Wait until the controller status register matches the requested state. */
static int fdc_wait_msr(uint8_t mask, uint8_t expected)
{
    uint64_t deadline = timer_monotonic_ns() + (uint64_t)FLOPPY_MSR_TIMEOUT_MS * 1000000ULL;
    for (;;) {
        if ((inb(FLOPPY_PORT_MSR) & mask) == expected) return EOK;
        if (timer_monotonic_ns() >= deadline) return -ETIMEDOUT;
    }
}

/* Send one command or parameter byte to the controller. */
static int fdc_write(uint8_t value)
{
    int status = fdc_wait_msr(FLOPPY_MSR_RQM | FLOPPY_MSR_DIO, FLOPPY_MSR_RQM);
    if (status != EOK) return status;

    outb(FLOPPY_PORT_FIFO, value);
    return EOK;
}

/* Read one result byte from the controller. */
static int fdc_read(uint8_t *value)
{
    int status;

    if (!value) return -EINVAL;
    status = fdc_wait_msr(FLOPPY_MSR_RQM | FLOPPY_MSR_DIO, FLOPPY_MSR_RQM | FLOPPY_MSR_DIO);

    if (status != EOK) return status;
    *value = inb(FLOPPY_PORT_FIFO);

    return EOK;
}

/* Wait for the IRQ handler to signal command completion. */
static int fdc_wait_irq(void)
{
    uint64_t deadline = timer_monotonic_ns() + (uint64_t)FLOPPY_IRQ_TIMEOUT_MS * 1000000ULL;
    for (;;) {
        if (__atomic_load_n(&fdc_irq_seen, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&fdc_irq_seen, 0, __ATOMIC_RELEASE);
            return EOK;
        }
        if (timer_monotonic_ns() >= deadline) return -ETIMEDOUT;
    }
}

/* Read and return the controller's pending interrupt status. */
static int fdc_sense_interrupt(uint8_t *st0, uint8_t *cylinder)
{
    int status = fdc_write(FLOPPY_CMD_SENSE_INTERRUPT); // SENSE INTERRUPT STATUS
    if (status != EOK) return status;
    if ((status = fdc_read(st0)) != EOK) return status;
    return fdc_read(cylinder);
}

/* Read a drive's status register 3 (head, media and write-protect lines). */
static int fdc_sense_drive(uint8_t drive, uint8_t *st3)
{
    int status;

    if (!st3) return -EINVAL;
    if ((status = fdc_write(FLOPPY_CMD_SENSE_DRIVE)) != EOK) return status;
    if ((status = fdc_write(drive & 3)) != EOK) return status;
    return fdc_read(st3);
}

/* Busy-wait for the reset settle time; called with fdc_lock held. */
static void fdc_udelay(uint32_t us)
{
    uint64_t deadline = timer_monotonic_ns() + (uint64_t)us * 1000ULL;
    while (timer_monotonic_ns() < deadline) __asm__ __volatile__("pause");
}

/* Reset the controller and restore its transfer configuration. */
static int fdc_reset(uint8_t selected_drive)
{
    uint8_t st0, cylinder;
    for (uint8_t i = 0; i < FLOPPY_MAX_DRIVES; i++) drives[i].calibrated = false;

    outb(FLOPPY_PORT_DOR, 0x00);
    fdc_udelay(100);
    __atomic_store_n(&fdc_irq_seen, 0, __ATOMIC_RELEASE);
    outb(FLOPPY_PORT_DOR, (uint8_t)(0x0c | (fdc_motor_mask << 4) | selected_drive)); // release reset, enable DMA and IRQ
    fdc_udelay(100);                                                                 // idle before the first command

    if (fdc_wait_irq() != EOK) return -ETIMEDOUT;
    for (uint8_t i = 0; i < 4; i++) {
        if (fdc_sense_interrupt(&st0, &cylinder) != EOK) return -EIO;
    }

    outb(FLOPPY_PORT_CCR, 0x00); // 500 kbit/s
    if (fdc_write(FLOPPY_CMD_SPECIFY) != EOK || fdc_write(0xdf) != EOK || fdc_write(0x02) != EOK) return -EIO;
    if (fdc_write(FLOPPY_CMD_CONFIGURE) != EOK || fdc_write(0x00) != EOK || fdc_write(0x57) != EOK || fdc_write(0x00) != EOK) return -EIO;

    fdc_ready = true;
    return EOK;
}

/* Move a drive's head to cylinder zero and synchronize the cached position. */
static int fdc_recalibrate(uint8_t drive)
{
    uint8_t st0, cylinder;

    __atomic_store_n(&fdc_irq_seen, 0, __ATOMIC_RELEASE);
    if (fdc_write(FLOPPY_CMD_RECALIBRATE) != EOK || fdc_write(drive & 3) != EOK) return -EIO;
    if (fdc_wait_irq() != EOK || fdc_sense_interrupt(&st0, &cylinder) != EOK) return -ETIMEDOUT;
    if ((st0 & 0xc0) != 0 || cylinder != 0) return -EIO;

    drives[drive].current_cylinder = 0;
    drives[drive].calibrated       = true;
    return EOK;
}

/* Seek to a cylinder and verify the controller's reported position. */
static int fdc_seek(uint8_t drive, uint8_t cylinder, uint8_t head)
{
    uint8_t st0, actual;
    if (drives[drive].current_cylinder == cylinder) return EOK;

    __atomic_store_n(&fdc_irq_seen, 0, __ATOMIC_RELEASE);
    if (fdc_write(FLOPPY_CMD_SEEK) != EOK || fdc_write((uint8_t)((head << 2) | drive)) != EOK || fdc_write(cylinder) != EOK) return -EIO;
    if (fdc_wait_irq() != EOK || fdc_sense_interrupt(&st0, &actual) != EOK) return -ETIMEDOUT;
    if ((st0 & 0xc0) != 0 || actual != cylinder) return -EIO;

    drives[drive].current_cylinder = cylinder;
    drives[drive].calibrated       = true;
    return EOK;
}

/* Sleep for ms milliseconds; the queue has no waker, so the deadline returns it. */
static void fdc_sleep_ms(wait_queue_t *queue, uint32_t ms)
{
    uint64_t deadline = sched_ticks() + timer_ns_to_ticks_ceil((uint64_t)ms * 1000000ULL);
    wait_queue_prepare(queue);
    (void)wait_queue_wait_timed(queue, deadline);
}

/* Start the selected drive's motor and wait for it to reach speed. */
static int fdc_motor_on(uint8_t drive)
{
    raw_spin_lock(&fdc_lock);
    if (!drives[drive].motor_on) {
        fdc_motor_mask |= (uint8_t)(1U << drive);
        outb(FLOPPY_PORT_DOR, (uint8_t)(0x0c | (fdc_motor_mask << 4) | drive));
        drives[drive].motor_on = true;
    }

    outb(FLOPPY_PORT_DOR, (uint8_t)(0x0c | (fdc_motor_mask << 4) | drive));
    drives[drive].last_use_ns = timer_monotonic_ns();
    bool needs_spinup         = !drives[drive].motor_ready;

    raw_spin_unlock(&fdc_lock);
    if (!needs_spinup) return EOK;

    /* The drive needs up to 500 ms to reach operating speed. */
    fdc_sleep_ms(&floppy_spinup_wait, FLOPPY_SPINUP_MS);
    raw_spin_lock(&fdc_lock);
    drives[drive].motor_ready = true;
    raw_spin_unlock(&fdc_lock);

    return EOK;
}

/* Stop a motor idle for FLOPPY_MOTOR_IDLE_MS; the next access spins it up again. */
static void fdc_motor_idle_scan(void)
{
    for (uint8_t i = 0; i < FLOPPY_MAX_DRIVES; i++) {
        raw_spin_lock(&fdc_lock);

        /* Under the lock: a transfer in flight refreshes last_use_ns. */
        if (drives[i].present && drives[i].motor_on && timer_monotonic_ns() - drives[i].last_use_ns >= (uint64_t)FLOPPY_MOTOR_IDLE_MS * 1000000ULL) {
            fdc_motor_mask &= (uint8_t) ~(1U << i);
            outb(FLOPPY_PORT_DOR, (uint8_t)(0x0c | (fdc_motor_mask << 4) | i));
            drives[i].motor_on    = false;
            drives[i].motor_ready = false;
        }
        raw_spin_unlock(&fdc_lock);
    }
}

/* Poll the motors once per FLOPPY_MOTOR_POLL_MS. */
static int floppy_motor_worker(void *arg)
{
    (void)arg;

    while (!kthread_should_stop()) {
        fdc_sleep_ms(&floppy_motor_wait, FLOPPY_MOTOR_POLL_MS);
        fdc_motor_idle_scan();
    }
    return 0;
}

/*
 * DIR.7 (cleared by reading DIR) flags a medium change.  The first read is a
 * baseline; one assertion costs one failed transfer so the layer above re-reads.
 */
static bool fdc_media_changed(uint8_t drive)
{
    bool latched = (inb(FLOPPY_PORT_DIR) & FLOPPY_DIR_DSKCHG) != 0;

    if (!drives[drive].media_known) {
        drives[drive].media_known   = true;
        drives[drive].media_latched = latched;
        return false;
    }
    if (!latched) {
        drives[drive].media_latched = false;
        return false;
    }
    if (drives[drive].media_latched) return false;

    drives[drive].media_latched = true;
    drives[drive].calibrated    = false;
    drives[drive].wp_known      = false; // the switch describes the new medium
    drives[drive].write_protect = false;

    plogk("floppy: media change detected on fd%u\n", drive);
    return true;
}

/* Cache ST3.6 after a successful read; before that the switch is meaningless and an empty drive would look write-protected. */
static void fdc_query_write_protect(uint8_t drive)
{
    uint8_t st3;

    if (drives[drive].wp_known) return;
    if (fdc_sense_drive(drive, &st3) != EOK) return;
    drives[drive].write_protect = (st3 & FLOPPY_ST3_WP) != 0;
    drives[drive].wp_known      = true;
}

/* Transfer one sector through the ISA DMA buffer. */
static int fdc_transfer(uint8_t drive, uint32_t lba, void *buffer, bool write)
{
    const floppy_geometry_t *geometry = floppy_get_geometry(drive);
    uint8_t                  cylinder, head, sector;
    uint8_t                  last_st[3] = {0xff, 0xff, 0xff};
    const char              *stage      = "command";
    bool                     have_st    = false;
    int                      status     = -EIO;

    if (!geometry || !buffer || lba >= geometry->sectors || !dma_buffer) return -EINVAL;
    cylinder = (uint8_t)(lba / (geometry->heads * geometry->sectors_per_track));
    head     = (uint8_t)((lba / geometry->sectors_per_track) % geometry->heads);
    sector   = (uint8_t)(lba % geometry->sectors_per_track + 1);

    /* A replaced medium fails here, before any motor spin-up. */
    if (fdc_media_changed(drive)) return -EIO;
    fdc_motor_on(drive);
    raw_spin_lock(&fdc_lock);

    if (!fdc_ready && fdc_reset(drive) != EOK) {
        stage = "reset";
        goto out;
    }
    outb(FLOPPY_PORT_CCR, geometry->data_rate);

    if (!drives[drive].calibrated && fdc_recalibrate(drive) != EOK) {
        stage = "recalibrate";
        goto out;
    }
    if (fdc_seek(drive, cylinder, head) != EOK) {
        fdc_ready = false;
        if (fdc_reset(drive) != EOK || fdc_recalibrate(drive) != EOK || fdc_seek(drive, cylinder, head) != EOK) {
            stage = "seek";
            goto out;
        }
    }

    for (int retry = 0; retry < FLOPPY_COMMAND_RETRIES; retry++) {
        uint8_t result[7]    = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        bool    result_valid = true;
        uint8_t command      = write ? FLOPPY_CMD_WRITE_DATA : FLOPPY_CMD_READ_DATA; /* MFM, single-track read/write */
        if (write) memcpy(dma_buffer, buffer, FLOPPY_SECTOR_SIZE);
        if (write)
            dma_send(FLOPPY_DMA_CHANNEL, (uint32_t *)(uintptr_t)dma_buffer_phys, FLOPPY_SECTOR_SIZE);
        else
            dma_recv(FLOPPY_DMA_CHANNEL, (uint32_t *)(uintptr_t)dma_buffer_phys, FLOPPY_SECTOR_SIZE);

        __atomic_store_n(&fdc_irq_seen, 0, __ATOMIC_RELEASE);
        if (fdc_write(command) != EOK || fdc_write((uint8_t)((head << 2) | drive)) != EOK || fdc_write(cylinder) != EOK || fdc_write(head) != EOK || fdc_write(sector) != EOK || fdc_write(2) != EOK
            || fdc_write(geometry->sectors_per_track) != EOK || fdc_write(0x1b) != EOK || fdc_write(0xff) != EOK)
            continue;
        if (fdc_wait_irq() != EOK) continue;
        for (size_t i = 0; i < sizeof(result); i++) {
            if (fdc_read(&result[i]) != EOK) {
                result_valid = false;
                break;
            }
        }
        if (result_valid) {
            last_st[0] = result[0];
            last_st[1] = result[1];
            last_st[2] = result[2];
            have_st    = true;
        }
        if (result_valid && (result[0] & 0xc0) == 0 && result[1] == 0 && result[2] == 0) {
            if (!write) {
                memcpy(buffer, dma_buffer, FLOPPY_SECTOR_SIZE);
                fdc_query_write_protect(drive);
            }
            status = EOK;
            break;
        }

        /* Clear the disk-change latch when present, then retry the command. */
        (void)inb(FLOPPY_PORT_DIR);
    }
out:
    raw_spin_unlock(&fdc_lock);

    /* Report a failed sector once per rate-limit window, with the FDC status. */
    if (status != EOK) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) {
            if (have_st)
                plogk("floppy: fd%u: %s lba %u failed in %s (%d, st0=0x%02x st1=0x%02x st2=0x%02x)\n", drive, write ? "write" : "read", lba, stage, status, last_st[0], last_st[1], last_st[2]);
            else
                plogk("floppy: fd%u: %s lba %u failed in %s (%d)\n", drive, write ? "write" : "read", lba, stage, status);
        }
    }
    return status;
}

/* Read sectors from the floppy through the generic block-device interface. */
static int floppy_read_sectors(const blockdev_device_t *dev, uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t                 *cursor = buffer;
    const floppy_geometry_t *geometry;
    if (!dev || !buffer || !(geometry = floppy_get_geometry(dev->drive))) return -ENODEV;
    if (lba >= geometry->sectors || count > geometry->sectors - lba) return -EINVAL;
    for (uint32_t i = 0; i < count; i++) {
        int status = fdc_transfer(dev->drive, (uint32_t)lba + i, cursor + (size_t)i * BLOCKDEV_SECTOR_SIZE, false);
        if (status != EOK) return status;
    }
    return EOK;
}

/* Write sectors to the floppy through the generic block-device interface. */
static int floppy_write_sectors(const blockdev_device_t *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    const uint8_t           *cursor = buffer;
    const floppy_geometry_t *geometry;
    if (!dev || !buffer || !(geometry = floppy_get_geometry(dev->drive))) return -ENODEV;
    if (lba >= geometry->sectors || count > geometry->sectors - lba) return -EINVAL;

    /* Known write-protect: refuse without touching the controller. */
    if (drives[dev->drive].wp_known && drives[dev->drive].write_protect) return -EROFS;
    for (uint32_t i = 0; i < count; i++) {
        int status = fdc_transfer(dev->drive, (uint32_t)lba + i, (void *)(cursor + (size_t)i * BLOCKDEV_SECTOR_SIZE), true);
        if (status != EOK) return status;
    }
    return EOK;
}

static struct blockdev_ops floppy_ops = {
    .read_sectors  = floppy_read_sectors,
    .write_sectors = floppy_write_sectors,
};

/* Create a block-device descriptor for one detected floppy drive. */
int blockdev_open_floppy(uint8_t drive, blockdev_device_t *device)
{
    const floppy_geometry_t *geometry = floppy_get_geometry(drive);
    if (!device || !geometry || floppy_ops_id < 0) return -ENODEV;
    memset(device, 0, sizeof(*device));
    device->ops_id       = (uint8_t)floppy_ops_id;
    device->drive        = drive;
    device->sector_size  = FLOPPY_SECTOR_SIZE;
    device->sector_count = geometry->sectors;

    /* Once the switch is known, let the block layer reject writes early. */
    device->read_only = drives[drive].wp_known && drives[drive].write_protect;
    return EOK;
}

/* Probe CMOS drive types and register available floppy disks. */
void floppy_init(void)
{
    uint8_t drive_types = read_cmos(0x10);
    uint8_t first       = drive_types >> 4;
    uint8_t second      = drive_types & 0x0f;

    /* No drive configured. */
    if (!first && !second) return;

    for (uint8_t i = 0; i < FLOPPY_MAX_DRIVES; i++) {
        uint8_t type = i == 0 ? first : second;
        if (type >= sizeof(floppy_geometries) / sizeof(floppy_geometries[0])) continue;
        drives[i].type    = (floppy_type_t)type;
        drives[i].present = type != 0;
    }
    if (!drives[0].present && !drives[1].present) return;

    /* The 8237 page register can only address the low 16 MiB of physical RAM. */
    dma_buffer_phys = alloc_frames_below(1, 0x01000000ULL);
    if (!dma_buffer_phys || (dma_buffer_phys & 0xffff) > 0xfe00) {
        if (dma_buffer_phys) free_frames(dma_buffer_phys, 1);
        dma_buffer_phys = 0;
        plogk("floppy: no DMA-capable buffer below 16 MiB; controller disabled.\n");
        return;
    }

    dma_buffer = phys_to_virt(dma_buffer_phys);
    if (!dma_buffer) {
        free_frames(dma_buffer_phys, 1);
        dma_buffer_phys = 0;
        plogk("floppy: cannot map the DMA buffer; controller disabled.\n");
        return;
    }

    register_interrupt_handler(IRQ_6, (void *)floppy_irq, 0, 0x8e);
    floppy_ops_id = blockdev_register_type(&floppy_ops);

    if (floppy_ops_id < 0) {
        plogk("floppy: cannot register block backend.\n");
        unregister_interrupt_handler(IRQ_6);
        free_frames(dma_buffer_phys, 1);
        dma_buffer_phys = 0;
        dma_buffer      = NULL;
        return;
    }

    plogk("floppy: FDC at 0x3f0-0x3f7, IRQ 6, DMA %u, bounce buffer at 0x%llx\n", FLOPPY_DMA_CHANNEL, (unsigned long long)dma_buffer_phys);
    for (uint8_t i = 0; i < FLOPPY_MAX_DRIVES; i++) {
        blockdev_device_t        device;
        const floppy_geometry_t *geometry = floppy_get_geometry(i);
        char                     name[8];
        if (!geometry || blockdev_open_floppy(i, &device) != EOK) continue;
        (void)snprintf(name, sizeof(name), "fd%u", i);
        if (block_register_disk(name, 2, i, &device, false, false) != EOK) {
            plogk("floppy: failed to publish %s\n", name);
            continue;
        }
        plogk("floppy: fd%u: CMOS type %u, %s, %u cyl x %u heads x %u spt, %u sectors.\n", i, (unsigned)geometry->type, floppy_type_names[geometry->type], geometry->cylinders, geometry->heads,
              geometry->sectors_per_track, geometry->sectors);
    }
}

/* Register the motor worker with main.c's unified worker start. */
void floppy_start_workers(void)
{
    wait_queue_init(&floppy_spinup_wait);
    wait_queue_init(&floppy_motor_wait);

    /* No drives, or the controller never came up: nothing will spin. */
    if (floppy_ops_id < 0) return;
    if (kernel_worker_register("floppy-motor", floppy_motor_worker, NULL, NULL) != EOK) plogk("floppy: cannot register the motor idle worker\n");
}

#endif
