/*
 *
 *      evdev.h
 *      Linux-compatible evdev input event subsystem header
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EVDEV_H_
#define INCLUDE_EVDEV_H_

#include <drivers/input_event.h>
#include <libs/glist/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <proc/task.h>
#include <sync/spin_lock.h>

/* ---- evdev internal constants ---- */
#define EVDEV_MINOR_BASE      64
#define EVDEV_MINORS          32
#define EVDEV_MIN_BUFFER_SIZE 64U
#define EVDEV_BUF_PACKETS     8
#define EVDEV_MAX_NAME_LEN    80

/* Maximum number of simultaneous evdev devices */
#define EVDEV_MAX_DEVICES 32

/* ---- input device descriptor (attached to each evdev) ---- */
/* This represents the physical/virtual input device capabilities */
typedef struct {
        char            name[EVDEV_MAX_NAME_LEN];              /* device name */
        char            phys[EVDEV_MAX_NAME_LEN];              /* physical path */
        char            uniq[EVDEV_MAX_NAME_LEN];              /* unique identifier */
        input_id_t      id;                                    /* bus/vendor/product/version */
        uint32_t        propbit[((INPUT_PROP_CNT) + 31) / 32]; /* properties bitmap */
        uint32_t        evbit[((EV_CNT) + 31) / 32];           /* supported event types */
        uint32_t        keybit[((KEY_CNT) + 31) / 32];         /* key codes bitmap */
        uint32_t        relbit[((REL_CNT) + 31) / 32];         /* relative axes bitmap */
        uint32_t        absbit[((ABS_CNT) + 31) / 32];         /* absolute axes bitmap */
        uint32_t        mscbit[((MSC_CNT) + 31) / 32];         /* misc codes bitmap */
        uint32_t        ledbit[((LED_CNT) + 31) / 32];         /* LED codes bitmap */
        uint32_t        sndbit[((SND_CNT) + 31) / 32];         /* sound codes bitmap */
        uint32_t        swbit[((SW_CNT) + 31) / 32];           /* switch codes bitmap */
        uint32_t        ffbit[((FF_CNT) + 31) / 32];           /* force feedback bitmap */
        input_absinfo_t absinfo[ABS_CNT];                      /* per-axis info */
        uint32_t        key_state[((KEY_CNT) + 31) / 32];      /* current key state */
        uint32_t        led_state[((LED_CNT) + 31) / 32];      /* current LED state */
        uint32_t        snd_state[((SND_CNT) + 31) / 32];      /* current sound state */
        uint32_t        sw_state[((SW_CNT) + 31) / 32];        /* current switch state */
        uint16_t        hint_events_per_packet;                /* hint for buffer sizing */
        int             rep[2];                                /* [0]=delay, [1]=period */
        bool            exist;                                 /* device is alive */
} input_dev_t;

/* ---- evdev client (one per open fd) ---- */
typedef struct evdev_client {
        unsigned int  head;        /* buffer write position */
        unsigned int  tail;        /* buffer read position */
        unsigned int  packet_head; /* first element of next packet */
        spinlock_t    buffer_lock; /* protects buffer, head, tail */
        wait_queue_t  wait;        /* wait queue for blocking reads */
        struct evdev *evdev;       /* back-pointer to evdev */
        ilist_node_t  node;        /* linkage in evdev->client_list */
        int           clk_type;    /* CLOCK_REALTIME / CLOCK_MONOTONIC / CLOCK_BOOTTIME */
        bool          revoked;     /* device access revoked */
        unsigned int  bufsize;     /* size of buffer[] array (power of 2) */
        /* Event filter masks */
        uint32_t *evmasks[EV_CNT];
        /* Flexible array member (What the hell?) */
        input_event_t buffer[];
} evdev_client_t;

/* ---- evdev device (one per input device) ---- */
typedef struct evdev {
        int             open_count;  /* number of open clients */
        input_dev_t    *input_dev;   /* associated input device */
        evdev_client_t *grab;        /* exclusive grab client (or NULL) */
        ilist_node_t    client_list; /* head of client list (ilist_node_t) */
        spinlock_t      client_lock; /* protects client_list */
        spinlock_t      mutex;       /* device-level mutex */
        bool            exist;       /* device is alive */
        int             minor;       /* assigned minor number */
} evdev_t;

/* ---- Public API ---- */

/* Allocate and initialize a new evdev device for the given input_dev.
 * The input_dev is NOT copied - caller must keep it alive.
 * Returns NULL on failure. */
evdev_t *evdev_create(input_dev_t *dev);

/* Destroy an evdev device. Hangs up all clients first. */
void evdev_destroy(evdev_t *evdev);

/* Register an evdev device into the global evdev table.
 * Returns 0 on success, negative errno on failure. */
int evdev_register(evdev_t *evdev);

/* Unregister and destroy an evdev device. */
void evdev_unregister(evdev_t *evdev);

/* Find an evdev by minor number. Returns NULL if not found. */
evdev_t *evdev_find_by_minor(int minor);

/* Initialize the evdev subsystem. Called once at boot. */
void evdev_init(void);

/* ---- Event injection API (called by input device drivers) ---- */

/* Inject a single event into the evdev subsystem.
 * This is the main entry point for input device drivers.
 * The event is passed to all clients of the evdev associated with dev. */
void evdev_inject_event(input_dev_t *dev, uint16_t type, uint16_t code, int32_t value);

/* Inject a SYN_REPORT event to flush the current packet. */
void evdev_inject_syn(input_dev_t *dev);

/* ---- File operation callbacks (for VFS integration) ---- */

/* These are the file_operations callbacks for /dev/input/eventX nodes.
 * The VFS layer should call these when a user process opens/reads/writes
 * the evdev device node.
 *
 * The 'ctx' parameter is the evdev_t pointer. */

/* Called on open(). Returns evdev_client_t pointer to store in file->private. */
evdev_client_t *evdev_fop_open(evdev_t *evdev);

/* Called on close(). client is file->private_data. */
void evdev_fop_release(evdev_client_t *client);

/* Called on read(). Returns bytes read or negative errno. */
ssize_t evdev_fop_read(evdev_client_t *client, void *buf, size_t count, bool nonblock);

/* Called on write(). Returns bytes written or negative errno. */
ssize_t evdev_fop_write(evdev_client_t *client, const void *buf, size_t count);

/* Called on poll/select. Returns poll flags mask. */
int evdev_fop_poll(evdev_client_t *client, int events);

/* Called on ioctl(). Returns 0 on success or negative errno. */
int evdev_fop_ioctl(evdev_client_t *client, uint32_t request, void *arg);

#endif /* INCLUDE_EVDEV_H_ */