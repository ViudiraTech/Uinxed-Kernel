/*
 *
 *      event.c
 *      ACPI event subsystem: SCI, fixed events, GPE, power button
 *
 *      2026/7/20 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/idt.h>
#include <chipset/common.h>
#include <drivers/acpi.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* PM1 register access via FADT                                       */
/* ------------------------------------------------------------------ */

static uint16_t pm1a_sts; /* PM1a status register port */
static uint16_t pm1b_sts; /* PM1b status register port */
static uint16_t pm1a_en;  /* PM1a enable register port */
static uint16_t pm1b_en;  /* PM1b enable register port */

static void pm1_setup(void)
{
    acpi_facp_t *f = get_acpi_facp();
    if (!f) return;

    /* Prefer the 64-bit extended address if present */
    if (f->x_pm1a_evt_blk.address && f->x_pm1a_evt_blk.bit_width >= 32) {
        pm1a_sts = (uint16_t)f->x_pm1a_evt_blk.address;
    } else {
        pm1a_sts = (uint16_t)f->pm1a_evt_blk;
    }
    if (f->x_pm1b_evt_blk.address && f->x_pm1b_evt_blk.bit_width >= 32) {
        pm1b_sts = (uint16_t)f->x_pm1b_evt_blk.address;
    } else {
        pm1b_sts = (uint16_t)f->pm1b_evt_blk;
    }

    /* Enable register is at offset PM1_EVT_LEN/2 within the block */
    uint8_t evt_len = f->pm1_evt_len;
    if (!evt_len) evt_len = 4;
    pm1a_en = pm1a_sts + evt_len / 2;
    pm1b_en = pm1b_sts ? pm1b_sts + evt_len / 2 : 0;
}

uint16_t acpi_pm1_status(void)
{
    uint16_t sts = inw(pm1a_sts);
    if (pm1b_sts) sts |= inw(pm1b_sts);
    return sts;
}

uint16_t acpi_pm1_enable(void)
{
    uint16_t en = inw(pm1a_en);
    if (pm1b_en) en |= inw(pm1b_en);
    return en;
}

void acpi_pm1_status_clear(uint16_t bits)
{
    outw(pm1a_sts, bits);
    if (pm1b_sts) outw(pm1b_sts, bits);
}

void acpi_pm1_enable_set(uint16_t bits)
{
    outw(pm1a_en, bits);
    if (pm1b_en) outw(pm1b_en, bits);
}

/* ------------------------------------------------------------------ */
/* Fixed event dispatch table                                         */
/* ------------------------------------------------------------------ */

#define ACPI_NUM_FIXED_EVENTS 3

static struct {
        acpi_event_callback_t handler;
        void                 *context;
        uint16_t              status_mask;
        uint16_t              enable_mask;
        const char           *name;
} fixed_events[ACPI_NUM_FIXED_EVENTS] = {
    [ACPI_EVENT_POWER_BUTTON] = {.status_mask = ACPI_PM1_STS_PWRBTN, .enable_mask = ACPI_PM1_EN_PWRBTN, .name = "power button"},
    [ACPI_EVENT_SLEEP_BUTTON] = {.status_mask = ACPI_PM1_STS_SLPBTN, .enable_mask = ACPI_PM1_EN_SLPBTN, .name = "sleep button"},
    [ACPI_EVENT_RTC]          = {.status_mask = ACPI_PM1_STS_RTC,    .enable_mask = ACPI_PM1_EN_RTC,    .name = "RTC"         },
};

int acpi_register_fixed_event(uint8_t event, acpi_event_callback_t handler, void *context)
{
    if (event >= ACPI_NUM_FIXED_EVENTS) return -1;
    fixed_events[event].handler = handler;
    fixed_events[event].context = context;
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPE dispatch                                                       */
/* ------------------------------------------------------------------ */

#define ACPI_MAX_GPE_HANDLERS 32

typedef struct {
        uint8_t               gpe_number;
        acpi_event_callback_t handler;
        void                 *context;
} gpe_handler_t;

static gpe_handler_t gpe_handlers[ACPI_MAX_GPE_HANDLERS];
static int           gpe_handler_count;

/* GPE block info decoded from FADT */
static struct {
        uint16_t block0_addr;
        uint8_t  block0_len;  /* in bytes */
        uint8_t  block0_base; /* base GPE number = 0 */
        uint16_t block1_addr;
        uint8_t  block1_len;  /* in bytes */
        uint8_t  block1_base; /* base GPE number */
} gpe_blocks;

static void gpe_setup(void)
{
    acpi_facp_t *f = get_acpi_facp();
    if (!f) return;

    if (f->x_gpe0_blk.address && f->x_gpe0_blk.bit_width >= 32) {
        gpe_blocks.block0_addr = (uint16_t)f->x_gpe0_blk.address;
    } else {
        gpe_blocks.block0_addr = (uint16_t)f->gpe0_blk;
    }
    gpe_blocks.block0_len  = f->gpe0_blk_len;
    gpe_blocks.block0_base = 0;

    if (f->x_gpe1_blk.address && f->x_gpe1_blk.bit_width >= 32) {
        gpe_blocks.block1_addr = (uint16_t)f->x_gpe1_blk.address;
    } else {
        gpe_blocks.block1_addr = (uint16_t)f->gpe1_blk;
    }
    gpe_blocks.block1_len  = f->gpe1_blk_len;
    gpe_blocks.block1_base = f->gpe1_base;
}

uint8_t acpi_gpe_status(uint8_t block_index)
{
    uint16_t addr = block_index ? gpe_blocks.block1_addr : gpe_blocks.block0_addr;
    if (!addr) return 0;
    return inb(addr);
}

void acpi_gpe_status_clear(uint8_t block_index, uint8_t bit)
{
    uint16_t addr = block_index ? gpe_blocks.block1_addr : gpe_blocks.block0_addr;
    if (!addr) return;
    outb(addr, 1 << bit);
}

/* Convert GPE number (0-255) to {block_index, bit_offset}. Returns -1 if out of range. */
static int gpe_number_to_bit(uint8_t gpe, uint8_t *block, uint8_t *bit)
{
    if (gpe < gpe_blocks.block0_base + gpe_blocks.block0_len * 8) {
        *block = 0;
        *bit   = gpe - gpe_blocks.block0_base;
        return 0;
    }
    if (gpe_blocks.block1_addr && gpe >= gpe_blocks.block1_base && gpe < gpe_blocks.block1_base + gpe_blocks.block1_len * 8) {
        *block = 1;
        *bit   = gpe - gpe_blocks.block1_base;
        return 0;
    }
    return -1;
}

int acpi_register_gpe(uint8_t gpe_number, acpi_event_callback_t handler, void *context)
{
    if (gpe_handler_count >= ACPI_MAX_GPE_HANDLERS) return -1;
    uint8_t block, bit;
    if (gpe_number_to_bit(gpe_number, &block, &bit)) return -1;

    gpe_handlers[gpe_handler_count].gpe_number = gpe_number;
    gpe_handlers[gpe_handler_count].handler    = handler;
    gpe_handlers[gpe_handler_count].context    = context;
    gpe_handler_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SCI interrupt handler                                              */
/* ------------------------------------------------------------------ */

static uint8_t sci_vector; /* IDT vector of the SCI */

/* Dispatch pending fixed events */
static void dispatch_fixed_events(uint16_t sts)
{
    for (int i = 0; i < ACPI_NUM_FIXED_EVENTS; i++) {
        if (sts & fixed_events[i].status_mask) {
            if (fixed_events[i].handler) { fixed_events[i].handler(fixed_events[i].context); }
        }
    }
}

/* Dispatch pending GPEs */
static void dispatch_gpes(void)
{
    for (int i = 0; i < gpe_handler_count; i++) {
        uint8_t block, bit;
        if (gpe_number_to_bit(gpe_handlers[i].gpe_number, &block, &bit)) continue;
        uint8_t sts = acpi_gpe_status(block);
        if (sts & (1 << bit)) {
            acpi_gpe_status_clear(block, bit);
            if (gpe_handlers[i].handler) { gpe_handlers[i].handler(gpe_handlers[i].context); }
        }
    }
}

/* Top-level SCI handler called from the IDT */
static void sci_handler(interrupt_frame_t *frame)
{
    (void)frame;

    uint16_t sts = acpi_pm1_status();
    if (!sts) {
        /* Spurious or GPE-only interrupt */
        dispatch_gpes();
        return;
    }

    /* Clear all pending fixed events */
    acpi_pm1_status_clear(sts);

    dispatch_fixed_events(sts);
    dispatch_gpes();
}

int acpi_sci_init(void)
{
    acpi_facp_t *f = get_acpi_facp();
    if (!f) return -1;

    /* Enable SCI-related fixed events.
     * The power-button enable is set; callers can add more. */
    acpi_pm1_enable_set(ACPI_PM1_EN_PWRBTN);

    /* Map ISA IRQ (FADT->sci_int) to the APIC vector.
     * Standard PC/AT routing: IRQ n -> vector (0x20 + n). */
    uint8_t irq = f->sci_int;
    if (irq == 0) irq = 9;
    sci_vector = IRQ_0 + irq;

    register_interrupt_handler(sci_vector, (void *)sci_handler, 0, 0x8e);
    plogk("acpi: SCI handler registered on vector %u (IRQ %u)\n", sci_vector, irq);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Poll entry (for systems without working SCI routing)               */
/* ------------------------------------------------------------------ */

void acpi_event_poll(void)
{
    uint16_t sts = acpi_pm1_status();
    uint16_t en  = acpi_pm1_enable();
    sts &= en;
    if (!sts) return;

    acpi_pm1_status_clear(sts);
    dispatch_fixed_events(sts);
    dispatch_gpes();
}

/* ------------------------------------------------------------------ */
/* Default power button callback                                      */
/* ------------------------------------------------------------------ */

static void power_button_handler(void *context)
{
    (void)context;
    plogk("acpi: Power button pressed, shutting down...\n");
    power_off();
}

/* ------------------------------------------------------------------ */
/* Public initializer                                                 */
/* ------------------------------------------------------------------ */

void acpi_event_init(void)
{
    acpi_facp_t *f = get_acpi_facp();
    if (!f) return;

    pm1_setup();
    gpe_setup();

    /* Register default power button handler */
    acpi_register_fixed_event(ACPI_EVENT_POWER_BUTTON, power_button_handler, 0);

    if (acpi_sci_init()) plogk("acpi: SCI init failed, falling back to polled events.\n");

    plogk("acpi: Event subsystem initialized.\n");
}
