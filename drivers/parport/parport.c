/*
 *
 *      parport.c
 *      Parallel port core (port registry and port access)
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/parport/parport.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

static parport_t *parport_list;
static spinlock_t parport_lock;
static int        parport_next_number;

int parport_register_port(const char *name, uint16_t base, int irq, void *private_data)
{
    parport_t *p;

    if (parport_find(base)) return -EEXIST;
    if (parport_count() >= PARPORT_MAX_PORTS) return -ENOSPC;

    p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;
    if (name)
        strncpy(p->name, name, sizeof(p->name) - 1);
    else
        (void)snprintf(p->name, sizeof(p->name), "parport%d", parport_next_number);
    p->number       = parport_next_number++;
    p->base         = base;
    p->irq          = irq;
    p->dev          = MKDEV(99, p->number);
    p->control      = 0x0c; /* INIT and SLCTIN asserted */
    p->private_data = private_data;

    spin_lock(&parport_lock);
    p->next      = parport_list;
    parport_list = p;
    spin_unlock(&parport_lock);

    plogk("parport: %s at 0x%03x (IRQ %d) registered.\n", p->name, base, irq);
    return 0;
}

void parport_unregister_port(parport_t *p)
{
    parport_t **link;

    if (!p) return;
    spin_lock(&parport_lock);
    link = &parport_list;
    while (*link && *link != p) link = &(*link)->next;
    if (*link) *link = p->next;
    spin_unlock(&parport_lock);
    free(p);
}

int parport_count(void)
{
    int        count = 0;
    parport_t *p;

    spin_lock(&parport_lock);
    for (p = parport_list; p; p = p->next) count++;
    spin_unlock(&parport_lock);
    return count;
}

parport_t *parport_get(int index)
{
    parport_t *p;
    int        i = 0;

    spin_lock(&parport_lock);
    for (p = parport_list; p && i < index; p = p->next) i++;
    if (p) p = p; /* keep address */
    spin_unlock(&parport_lock);
    return p;
}

parport_t *parport_find(uint16_t base)
{
    parport_t *p;

    spin_lock(&parport_lock);
    for (p = parport_list; p; p = p->next)
        if (p->base == base) break;
    spin_unlock(&parport_lock);
    return p;
}

parport_t *parport_find_by_number(int number)
{
    parport_t *p;

    spin_lock(&parport_lock);
    for (p = parport_list; p; p = p->next)
        if (p->number == number) break;
    spin_unlock(&parport_lock);
    return p;
}

/* ---- port access ---- */

uint8_t parport_read_data(parport_t *p)
{
    return p && p->read_data ? p->read_data(p) : 0;
}

void parport_write_data(parport_t *p, uint8_t v)
{
    if (p && p->write_data) p->write_data(p, v);
}

uint8_t parport_read_status(parport_t *p)
{
    return p && p->read_status ? p->read_status(p) : 0;
}

uint8_t parport_read_control(parport_t *p)
{
    if (p && p->read_control) return p->read_control(p);
    return p ? p->control : 0;
}

void parport_write_control(parport_t *p, uint8_t v)
{
    if (!p) return;
    p->control = v;
    if (p->write_control) p->write_control(p, v);
}

void parport_frob_control(parport_t *p, uint8_t mask, uint8_t v)
{
    if (!p) return;
    if (p->frob_control) {
        p->frob_control(p, mask, v);
        return;
    }
    parport_write_control(p, (parport_read_control(p) & ~mask) | (v & mask));
}

void parport_data_reverse(parport_t *p, bool reverse)
{
    if (!p) return;
    if (reverse)
        parport_frob_control(p, 0x20, 0x20);
    else
        parport_frob_control(p, 0x20, 0x00);
}
