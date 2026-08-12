/*
 *
 *      host.c
 *      USB Host Controller Driver abstraction layer
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/usb/host/host.h>
#include <drivers/usb/host/ohci/ohci.h>
#include <drivers/usb/host/uhci/uhci.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

usb_host_t     *usb_host_list;
spinlock_t      usb_host_lock;
static uint16_t usb_host_next_bus_number = 1;

/* Allocate the next unique USB bus number. */
int usb_host_allocate_bus_number(void)
{
    uint64_t flags = spin_lock_irqsave(&usb_host_lock);
    if (usb_host_next_bus_number > UINT8_MAX) {
        spin_unlock_irqrestore(&usb_host_lock, flags);
        return -ENOSPC;
    }
    int bus_number = usb_host_next_bus_number++;
    spin_unlock_irqrestore(&usb_host_lock, flags);
    return bus_number;
}

/* Add a host controller to the global list, rejecting duplicate buses. */
int usb_host_register(usb_host_t *host)
{
    if (!host || !host->bus_number) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&usb_host_lock);
    for (usb_host_t *entry = usb_host_list; entry; entry = entry->next) {
        if (entry->bus_number == host->bus_number) {
            spin_unlock_irqrestore(&usb_host_lock, flags);
            return -EEXIST;
        }
    }
    host->next    = usb_host_list;
    usb_host_list = host;
    spin_unlock_irqrestore(&usb_host_lock, flags);
    plogk("usb-host: Registered %s (bus %u, type %d)\n", host->name, host->bus_number, host->type);
    return EOK;
}

/* Remove a host controller from the global list. */
int usb_host_unregister(usb_host_t *host)
{
    if (!host) return -EINVAL;
    uint64_t     flags = spin_lock_irqsave(&usb_host_lock);
    usb_host_t **prev  = &usb_host_list;
    while (*prev) {
        if (*prev == host) {
            *prev = host->next;
            spin_unlock_irqrestore(&usb_host_lock, flags);
            return EOK;
        }
        prev = &(*prev)->next;
    }
    spin_unlock_irqrestore(&usb_host_lock, flags);
    return -ENODEV;
}

/* Look up a host controller by its bus number. */
usb_host_t *usb_host_find_by_bus(uint8_t bus_number)
{
    uint64_t flags = spin_lock_irqsave(&usb_host_lock);
    for (usb_host_t *host = usb_host_list; host; host = host->next) {
        if (host->bus_number == bus_number) {
            spin_unlock_irqrestore(&usb_host_lock, flags);
            return host;
        }
    }
    spin_unlock_irqrestore(&usb_host_lock, flags);
    return NULL;
}

/* Look up the index-th host controller of a given type. */
usb_host_t *usb_host_find_by_type(usb_host_type_t type, int index)
{
    uint64_t flags = spin_lock_irqsave(&usb_host_lock);
    int      count = 0;
    for (usb_host_t *host = usb_host_list; host; host = host->next) {
        if (host->type == type) {
            if (count == index) {
                spin_unlock_irqrestore(&usb_host_lock, flags);
                return host;
            }
            count++;
        }
    }
    spin_unlock_irqrestore(&usb_host_lock, flags);
    return NULL;
}

/* Return the number of registered host controllers. */
static int usb_host_get_count(void)
{
    uint64_t flags = spin_lock_irqsave(&usb_host_lock);
    int      count = 0;
    for (usb_host_t *host = usb_host_list; host; host = host->next) count++;
    spin_unlock_irqrestore(&usb_host_lock, flags);
    if (count < 0) count = 0;
    return count;
}

/* Probe the PCI bus for all supported USB host controllers. */
void usb_host_pci_scan(void)
{
    plogk("usb-host: Scanning PCI for USB host controllers...\n");
    xhci_init();
    ehci_init();
    ohci_init();
    uhci_init();
    plogk("usb-host: Found %d controller(s)\n", usb_host_get_count());
}

/* Start every registered controller and its hub worker. */
void usb_host_start_workers(void)
{
    uint64_t flags = spin_lock_irqsave(&usb_host_lock);
    for (usb_host_t *host = usb_host_list; host; host = host->next) {
        if (host->controller_ops && host->controller_ops->host_start && !host->running) {
            int ret = host->controller_ops->host_start(host);
            if (ret == EOK) {
                host->running = true;
            } else {
                plogk("usb-host: %s: start failed (%d)\n", host->name[0] ? host->name : "controller", ret);
            }
        }
    }
    spin_unlock_irqrestore(&usb_host_lock, flags);
    xhci_start_workers();
    uhci_start_workers();
    ohci_start_workers();
    ehci_start_workers();
}

/* Shut down every registered host controller. */
void usb_host_shutdown_all(void)
{
    xhci_shutdown();
    uint64_t flags = spin_lock_irqsave(&usb_host_lock);
    for (usb_host_t *host = usb_host_list; host; host = host->next) {
        if (host->running && host->controller_ops && host->controller_ops->host_stop) host->controller_ops->host_stop(host);
        host->running = false;
    }
    spin_unlock_irqrestore(&usb_host_lock, flags);
}
