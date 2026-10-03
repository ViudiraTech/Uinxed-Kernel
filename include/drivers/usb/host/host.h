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

typedef enum {
    USB_HOST_UHCI = 0,
    USB_HOST_OHCI,
    USB_HOST_EHCI,
    USB_HOST_XHCI,
} usb_host_type_t;

struct usb_host;

typedef struct usb_host_controller_ops {
        void (*host_stop)(struct usb_host *host);
} usb_host_controller_ops_t;

typedef struct usb_host {
        usb_host_type_t            type;
        uint8_t                    bus_number;
        uint8_t                    max_ports;
        usb_host_controller_ops_t *controller_ops;
        const usb_hcd_ops_t       *hcd_ops;
        void                      *hc_private;
        struct usb_host           *next;
        bool                       running;
        char                       name[16];
} usb_host_t;

extern usb_host_t *usb_host_list;

/* Add a host controller to the global list; duplicates are reported, not returned. */
void usb_host_register(usb_host_t *host);

/* Remove a host controller from the global list. */
int usb_host_unregister(usb_host_t *host);

/* Allocate the next unique USB bus number. */
int usb_host_allocate_bus_number(void);

/* Allocate a free device address on a bus (1..127). */
int usb_host_allocate_address(uint8_t bus_number, uint8_t *address);

/* Release a device address on a bus. */
void usb_host_release_address(uint8_t bus_number, uint8_t address);

#if CONFIG_USB

/* Probe the PCI bus for all supported USB host controllers. */
void usb_host_pci_scan(void);

/* Start every registered controller and its hub worker. */
void usb_host_start_workers(void);

/* Stop and tear down every registered controller. */
void usb_host_shutdown_all(void);

#else
static inline void usb_host_pci_scan(void) {}
static inline void usb_host_start_workers(void) {}
static inline void usb_host_shutdown_all(void) {}
#endif

/* Look up a host controller by its bus number. */
usb_host_t *usb_host_find_by_bus(uint8_t bus_number);

/* Look up the index-th host controller of a given type. */
usb_host_t *usb_host_find_by_type(usb_host_type_t type, int index);

#if CONFIG_USB_UHCI && CONFIG_USB
int  uhci_init(void);
void uhci_start_workers(void);
void uhci_shutdown(void);
#else
static inline int uhci_init(void)
{
    return 0;
}
static inline void uhci_start_workers(void) {}
static inline void uhci_shutdown(void) {}
#endif

#if CONFIG_USB_OHCI && CONFIG_USB
int  ohci_init(void);
void ohci_start_workers(void);
void ohci_shutdown(void);
#else
static inline int ohci_init(void)
{
    return 0;
}
static inline void ohci_start_workers(void) {}
static inline void ohci_shutdown(void) {}
#endif

#if CONFIG_USB_EHCI && CONFIG_USB
int  ehci_init(void);
void ehci_start_workers(void);
void ehci_shutdown(void);
#else
static inline int ehci_init(void)
{
    return 0;
}
static inline void ehci_start_workers(void) {}
static inline void ehci_shutdown(void) {}
#endif

#if CONFIG_USB_XHCI && CONFIG_USB
int  xhci_init(void);
void xhci_start_workers(void);
void xhci_shutdown(void);
#else
static inline int xhci_init(void)
{
    return 0;
}
static inline void xhci_start_workers(void) {}
static inline void xhci_shutdown(void) {}
#endif

#endif // INCLUDE_HOST_H_
