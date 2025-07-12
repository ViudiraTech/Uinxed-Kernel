/*
 *
 *      main.c
 *      Uinxed kernel entry
 *
 *      2024/6/23 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "acpi.h"
#include "cmdline.h"
#include "common.h"
#include "cpu.h"
#include "debug.h"
#include "frame.h"
#include "gdt.h"
#include "heap.h"
#include "hhdm.h"
#include "ide.h"
#include "idt.h"
#include "interrupt.h"
#include "limine.h"
#include "page.h"
#include "pci.h"
#include "printk.h"
#include "serial.h"
#include "smbios.h"
#include "smp.h"
#include "uinxed.h"
#include "video.h"

/* Kernel entry */
void kernel_entry(void)
{
    video_init(); // Initialize Video
    page_init();  // Initialize memory page
    init_heap();  // Initialize the memory heap

    plogk("Uinxed version %s (%s version %s) #1 SMP %s %s\n", KERNL_VERS, COMPILER_NAME, COMPILER_VERSION, BUILD_DATE,
          BUILD_TIME);
    plogk("Framebuffer %p, resolution = %llux%llu\n", get_framebuffer()->address, get_framebuffer()->width,
          get_framebuffer()->height);
    plogk("Command line: %s\n", get_cmdline());
    plogk("SMBIOS %d.%d.0 present.\n", smbios_major_version(), smbios_minor_version());
    plogk("CPU: %s %s\n", get_vendor_name(), get_model_name());
    plogk("CPU: phy/virt = %u/%u bits.\n", get_cpu_phys_bits(), get_cpu_virt_bits());
    plogk("CPU: NX (Execute Disable) protection = %s\n", cpu_supports_nx() ? "active" : "passive");
    plogk("HEAP: Range = 0x%016llx - 0x%016llx (%lld MiB)\n", phys_to_virt(heap_start),
          phys_to_virt(heap_start + heap_size), heap_size / 1024 / 1024);
    init_gdt();           // Initialize global descriptors
    init_idt();           // Initialize interrupt descriptor
    isr_registe_handle(); // Register ISR interrupt processing
    acpi_init();          // Initialize ACPI
    smp_init();           // Initialize SMP
    print_memory_map();   // Print memory map information
    init_frame();         // Initialize memory frame
    pci_init();           // Initialize PCI
    init_ide();           // Initialize ATA/ATAPI driver
    init_serial();        // Initialize the serial port
    enable_intr();

    panic("No operation.");
}
