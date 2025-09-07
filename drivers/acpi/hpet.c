/*
 *
 *      hpet.c
 *      High-precision event timer
 *
 *      2025/2/16 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "acpi.h"
#include "apic.h"
#include "hhdm.h"
#include "idt.h"
#include "printk.h"
#include "scheduler.h"
#include "stdint.h"

hpet_info_t    *hpet_addr;
static uint32_t hpet_period = 0;

void timer_handle_c(regs_t *reg)
{
    interrupt_frame_t *frame = &((regs_t_ *)reg)->auto_regs.frame;
    if (is_scheduler[get_current_cpu_id()] == 0) { goto end; }

    if (current_task == NULL) { goto az; }

    if (current_task->flag & PCB_FLAGS_SWITCH_TO_USER) {
        frame->rip = current_task->context0.rip;
        current_task->flag ^= PCB_FLAGS_SWITCH_TO_USER;
        current_task->flag ^= PCB_FLAGS_KTHREAD;
        frame->cs = 0x20;
        frame->ss = 0x18;
        goto end;
    } else {
        current_task->context0.rip = frame->rip;
    }
az:
    scheduler(frame, reg);
end:
    send_eoi();
    return;
}

__attribute__((naked)) void timer_handle(__attribute__((unused)) interrupt_frame_t *frame)
{
    __asm__ volatile("cli\n\t"                 // disable interrupts
                     save_regs_asm_            // save registers
                     "mov %rsp, %rdi\n\t"      // 1st arg: interrupt frame
                     "call timer_handle_c\n\t" // call C function
                     restore_regs_asm_         // restore registers
                     "sti\n\t"                 // enable interrupts
                     "iretq\n\t");             // return from interrupt
}

/* Returns the nanosecond value of the current time */
uint64_t nano_time(void)
{
    if (!hpet_addr) return 0;
    return (hpet_addr->main_counter_value) * hpet_period;
}

/* Initialize high-precision event timer */
void hpet_init(hpet_t *hpet)
{
    hpet_addr = phys_to_virt(hpet->base_address.address);
    plogk("hpet: HPET base mapped to virtual address %p\n", hpet_addr);

    uint32_t counter_clock_period = hpet_addr->general_capabilities >> 32;
    hpet_period                   = counter_clock_period / 1000000;
    hpet_addr->main_counter_value = 0;

    plogk("hpet: HPET main counter is initialized to 0\n");
    plogk("hpet: HPET counter clock period = %u (ns)\n", counter_clock_period);
    plogk("hpet: HPET timer period = %u (us)\n", hpet_period);

    hpet_addr->general_configuration |= 1;
    register_interrupt_handler(IRQ_0, (void *)timer_handle, 0, 0x8e);
    plogk("hpet: HPET general configuration register set to 0x%08llx\n", hpet_addr->general_configuration);
}
