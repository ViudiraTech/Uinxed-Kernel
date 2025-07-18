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
#include "page.h"
#include "parallel.h"
#include "pci.h"
#include "printk.h"
#include "serial.h"
#include "smbios.h"
#include "smp.h"
#include "uinxed.h"
#include "video.h"

/* Executable entry */
void executable_entry(void)
{
    const char msg[] = "Logically you should use Limine to boot it instead of executing it directly, right?\n";
    __asm__ volatile("mov $1, %%rax\n"
                     "mov $1, %%rdi\n"
                     "lea %[msg], %%rsi\n"
                     "mov %[len], %%rdx\n"
                     "syscall\n"
                     "mov $60, %%rax\n"
                     "mov $1, %%rdi\n"
                     "syscall\n"
                     :
                     : [msg] "m"(msg), [len] "r"(sizeof msg - 1)
                     : "rax", "rdi", "rsi", "rdx");
}

/* Kernel entry */
void kernel_entry(void)
{
    video_init(); // Initialize Video
    page_init();  // Initialize memory page
    init_heap();  // Initialize the memory heap

    video_info_t fbinfo = video_get_info();

    plogk("Uinxed version %s (%s version %s) #1 SMP %s %s\n", KERNL_VERS, COMPILER_NAME, COMPILER_VERSION, BUILD_DATE,
          BUILD_TIME);
    plogk("fb0: Base address %p, Size %lu KiB.\n", fbinfo.framebuffer,
          (fbinfo.width * fbinfo.height * fbinfo.bpp) / (uint64_t)(8 * 1024));
    plogk("fb0: Mode %lux%lu @ %ubpp.\n", fbinfo.width, fbinfo.height, fbinfo.bpp);
    plogk("fb0: Color map: RGB, Mask bits R:%u G:%u B:%u\n", fbinfo.red_mask_size, fbinfo.green_mask_size,
          fbinfo.blue_mask_size);
    plogk("fb0: Channel offsets R:%u G:%u B:%u\n", fbinfo.red_mask_shift, fbinfo.green_mask_shift,
          fbinfo.blue_mask_shift);
    plogk("fbcon: fb0 is primary device.\n");
    plogk("fbcon: Screen grid: %lux%lu characters.\n", fbinfo.c_width, fbinfo.c_height);
    plogk("Command line: %s\n", get_cmdline());
    plogk("SMBIOS %d.%d.0 present.\n", smbios_major_version(), smbios_minor_version());
    plogk("cpu: Vendor: %s, Model: %s\n", get_vendor_name(), get_model_name());
    plogk("cpu: phy/virt = %u/%u Bits.\n", get_cpu_phys_bits(), get_cpu_virt_bits());
    plogk("cpu: NX (Execute Disable) protection = %s\n", cpu_supports_nx() ? "active" : "passive");
    plogk("page: kernel_page_dir = %p\n", get_kernel_pagedir());
    plogk("page: kernel_page_table = %p\n", phys_to_virt(get_cr3()));
    plogk("heap: Range: %p - %p (%llu MiB)\n", phys_to_virt(heap_start), phys_to_virt(heap_start + heap_size),
          heap_size / 1024 / 1024);
    plogk("x86/PAT: Configuration [0-7]: %s\n", get_pat_config().pat_str);
    plogk("dmi: %s %s, BIOS %s %s\n", smbios_sys_manufacturer(), smbios_sys_product_name(), smbios_bios_version(),
          smbios_bios_release_date());

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
    init_parallel();      // Initialize the parallel port
    enable_intr();

    panic("No operation.");
}
