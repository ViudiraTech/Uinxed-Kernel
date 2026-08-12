/*
 *
 *      host.h
 *      USB Host Controller Driver abstraction layer
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_HOST_H_
#define INCLUDE_HOST_H_

#include <drivers/bus/pci.h>
#include <drivers/usb/core/usb.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

#define USB_HOST_MAX_CONTROLLERS 16

typedef enum {
    USB_HOST_UHCI = 0,
    USB_HOST_OHCI,
    USB_HOST_EHCI,
    USB_HOST_XHCI,
} usb_host_type_t;

struct usb_host;

typedef struct usb_host_controller_ops {
        int (*host_start)(struct usb_host *host);
        void (*host_stop)(struct usb_host *host);
        int (*port_reset)(struct usb_host *host, uint8_t port);
        int (*port_speed)(struct usb_host *host, uint8_t port);
        int (*port_connected)(struct usb_host *host, uint8_t port);
} usb_host_controller_ops_t;

typedef struct usb_host {
        usb_host_type_t            type;
        uint8_t                    bus_number;
        uint8_t                    max_ports;
        uint8_t                    irq_vector;
        pci_device_cache_t        *pci_dev;
        usb_host_controller_ops_t *controller_ops;
        const usb_hcd_ops_t       *hcd_ops;
        void                      *hc_private;
        struct usb_host           *next;
        bool                       running;
        char                       name[16];
} usb_host_t;

extern usb_host_t *usb_host_list;
extern spinlock_t  usb_host_lock;

/* Add a host controller to the global list, rejecting duplicate buses. */
int usb_host_register(usb_host_t *host);

/* Remove a host controller from the global list. */
int usb_host_unregister(usb_host_t *host);

/* Allocate the next unique USB bus number. */
int usb_host_allocate_bus_number(void);

/* Probe the PCI bus for all supported USB host controllers. */
void usb_host_pci_scan(void);

/* Start every registered controller and its hub worker. */
void usb_host_start_workers(void);

/* Stop and tear down every registered controller. */
void usb_host_shutdown_all(void);

/* Look up a host controller by its bus number. */
usb_host_t *usb_host_find_by_bus(uint8_t bus_number);

/* Look up the index-th host controller of a given type. */
usb_host_t *usb_host_find_by_type(usb_host_type_t type, int index);

/* Per-controller PCI probing and driver entry points. */
void uhci_init(void);
void ohci_init(void);
void ehci_init(void);
void xhci_init(void);
void xhci_start_workers(void);
void xhci_shutdown(void);
void uhci_start_workers(void);
void ohci_start_workers(void);
void ehci_start_workers(void);
void uhci_shutdown(void);
void ohci_shutdown(void);
void ehci_shutdown(void);

#endif // INCLUDE_HOST_H_
