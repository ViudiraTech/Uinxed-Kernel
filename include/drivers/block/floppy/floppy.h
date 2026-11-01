/*
 *
 *      floppy.h
 *      PC floppy disk controller definitions
 *
 *      2026/11/1 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FLOPPY_H_
#define INCLUDE_FLOPPY_H_

#include <drivers/block/core/blockdev.h>
#include <libs/std/errno.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

/* Drive and sector limits exposed by the block-device interface. */
#define FLOPPY_MAX_DRIVES  2
#define FLOPPY_SECTOR_SIZE BLOCKDEV_SECTOR_SIZE

/* ISA DMA channel and controller timing limits. */
#define FLOPPY_DMA_CHANNEL 2

/* Register handshakes are immediate; a recalibrate or read is mechanical (~200 ms). */
#define FLOPPY_MSR_TIMEOUT_MS  100U
#define FLOPPY_IRQ_TIMEOUT_MS  2000U
#define FLOPPY_COMMAND_RETRIES 3

/* Motor management: spin-up time, and how long a motor may stay idle. */
#define FLOPPY_SPINUP_MS     500U
#define FLOPPY_MOTOR_IDLE_MS 3000U
#define FLOPPY_MOTOR_POLL_MS 1000U

/* Legacy PC/AT floppy disk controller I/O ports. */
#define FLOPPY_PORT_DOR  0x3f2
#define FLOPPY_PORT_MSR  0x3f4
#define FLOPPY_PORT_FIFO 0x3f5
#define FLOPPY_PORT_CCR  0x3f7
#define FLOPPY_PORT_DIR  0x3f7

/* Main Status Register bits. */
#define FLOPPY_MSR_RQM  0x80
#define FLOPPY_MSR_DIO  0x40
#define FLOPPY_MSR_BUSY 0x10

/* Digital Input Register bits (read-only; shares the CCR port address). */
#define FLOPPY_DIR_DSKCHG 0x80 // disk-change latch, cleared by reading DIR

/* Controller commands used by this driver. */
#define FLOPPY_CMD_SPECIFY         0x03
#define FLOPPY_CMD_SENSE_DRIVE     0x04
#define FLOPPY_CMD_SENSE_INTERRUPT 0x08
#define FLOPPY_CMD_RECALIBRATE     0x07
#define FLOPPY_CMD_SEEK            0x0f
#define FLOPPY_CMD_CONFIGURE       0x13
#define FLOPPY_CMD_READ_DATA       0x46
#define FLOPPY_CMD_WRITE_DATA      0x45

/* Status register 3 (SENSE DRIVE STATUS) bits. */
#define FLOPPY_ST3_WP 0x40 // write-protect switch is active

/* CMOS drive type values. */
typedef enum {
    FLOPPY_TYPE_NONE       = 0,
    FLOPPY_TYPE_360K       = 1,
    FLOPPY_TYPE_1200K      = 2,
    FLOPPY_TYPE_720K       = 3,
    FLOPPY_TYPE_1440K      = 4,
    FLOPPY_TYPE_2880K_5_25 = 5,
    FLOPPY_TYPE_2880K_3_5  = 6,
} floppy_type_t;

/* CHS and data-rate parameters for a supported disk format. */
typedef struct floppy_geometry {
        floppy_type_t type;
        uint16_t      cylinders;
        uint8_t       heads;
        uint8_t       sectors_per_track;
        uint8_t       data_rate;
        uint32_t      sectors;
} floppy_geometry_t;

/* Per-drive media geometry, head position, and motor/calibration state. */
typedef struct {
        floppy_type_t type;
        uint8_t       current_cylinder;
        bool          present;
        bool          motor_on;
        bool          motor_ready;
        bool          calibrated;
        bool          media_known;   // disk-change latch has a baseline to compare against
        bool          media_latched; // the latched change has already been reported
        bool          wp_known;      // write-protect switch of the current medium is known
        bool          write_protect; // ... and it is active
        uint64_t      last_use_ns;   // last motor use, for the idle timeout
} floppy_drive_t;

#if CONFIG_FLOPPY

/* Initialize the controller and register each detected drive. */
void floppy_init(void);

/* Register the driver's worker; init/main.c owns the single creation site. */
void floppy_start_workers(void);

/* Report whether a supported drive type was found in CMOS. */
bool floppy_drive_present(uint8_t drive);

/* Return the configured disk geometry for a detected drive. */
const floppy_geometry_t *floppy_get_geometry(uint8_t drive);

/* Open a floppy drive through the generic block-device layer. */
int blockdev_open_floppy(uint8_t drive, blockdev_device_t *device);

#else
static inline void floppy_init(void) {}
static inline void floppy_start_workers(void) {}
static inline bool floppy_drive_present(uint8_t)
{
    return false;
}
static inline const floppy_geometry_t *floppy_get_geometry(uint8_t)
{
    return NULL;
}
static inline int blockdev_open_floppy(uint8_t, blockdev_device_t *)
{
    return -ENODEV;
}
#endif

#endif // INCLUDE_FLOPPY_H_
