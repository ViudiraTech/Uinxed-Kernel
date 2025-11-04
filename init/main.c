/*
 *
 *      main.c
 *      Uinxed-kernel entry
 *
 *      2024/6/23 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include "acpi.h"
#include "cmdline.h"
#include "common.h"
#include "cpio.h"
#include "cpuid.h"
#include "debug.h"
#include "eis.h"
#include "frame.h"
#include "gdt.h"
#include "gfx_truetype.h"
#include "heap.h"
#include "hhdm.h"
#include "ide.h"
#include "interrupt.h"
#include "limine_module.h"
#include "page.h"
#include "parallel.h"
#include "pci.h"
#include "printk.h"
#include "ps2.h"
#include "serial.h"
#include "smbios.h"
#include "smp.h"
#include "tmpfs.h"
#include "tsc.h"
#include "uinxed.h"
#include "vfs.h"
#include "video.h"

/* Executable entry */
void executable_entry(void)
{
    const char msg[] = "Logically you should use Limine to boot it instead of executing it directly, right?\n\n";
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
    init_fpu(); // Initialize FPU/MMX
    init_sse(); // Initialize SSE/SSE2
    init_avx(); // Initialize AVX/AVX2

    init_frame();   // Initialize memory frame
    page_init();    // Initialize memory page
    init_heap();    // Initialize the memory heap
    lmodule_init(); // Initialize the passed-in resource module list

#if TTF_CONSOLE
    init_ttf(get_lmodule("UbuntuSansMono")->data); // Load ttf font
#endif

    video_init();                           // Initialize Video
    video_info_t fbinfo = video_get_info(); // Get video info

    plogk("%s version %s (%s version %s) SMP %s %s\n", KERNEL_NAME, KERNEL_VERSION, COMPILER_NAME, COMPILER_VERSION, BUILD_DATE, BUILD_TIME);
    plogk("fb0: Base %p, Size %lu KiB.\n", fbinfo.framebuffer, (fbinfo.width * fbinfo.height * fbinfo.bpp) / (uint64_t)(8 * 1024));
    plogk("fb0: Mode %lux%lu @ %ubpp.\n", fbinfo.width, fbinfo.height, fbinfo.bpp);
    plogk("fb0: Color map: RGB, Mask bits R:%u G:%u B:%u\n", fbinfo.red_mask_size, fbinfo.green_mask_size, fbinfo.blue_mask_size);
    plogk("fb0: Channel offsets R:%u G:%u B:%u\n", fbinfo.red_mask_shift, fbinfo.green_mask_shift, fbinfo.blue_mask_shift);
    plogk("fbcon: fb0 is primary device.\n");
    plogk("fbcon: Screen grid: %lux%lu characters.\n", fbinfo.c_width, fbinfo.c_height);
    plogk("Command line: %s\n", get_cmdline());
    plogk("SMBIOS %d.%d.0 present.\n", smbios_major_version(), smbios_minor_version());
    plogk("cpu: Vendor: %s, Model: %s\n", get_vendor_name(), get_model_name());
    plogk("cpu: phy/virt = %u/%u Bits.\n", get_cpu_phys_bits(), get_cpu_virt_bits());
    plogk("cpu: NX (Execute Disable) protection = %s\n", cpu_supports_nx() ? "active" : "passive");
    plogk("page: kernel_page_dir = %p\n", get_kernel_pagedir());
    plogk("page: kernel_page_table = %p\n", phys_to_virt(get_cr3()));
    plogk("heap: Range: %p - %p (%llu KiB)\n", KERNEL_HEAP_START, KERNEL_HEAP_START + KERNEL_HEAP_SIZE, KERNEL_HEAP_SIZE / 1024);
    plogk("x86/PAT: Configuration [0-7]: %s\n", get_pat_config().pat_str);
    plogk("dmi: %s %s, BIOS %s %s\n", smbios_sys_manufacturer(), smbios_sys_product_name(), smbios_bios_version(), smbios_bios_release_date());

    init_gdt();                     // Initialize global descriptors
    init_idt();                     // Initialize interrupt descriptor
    isr_registe_handle();           // Register ISR interrupt processing
    acpi_init();                    // Initialize ACPI
    tsc_init();                     // Initialize TSC
    smp_init();                     // Initialize SMP
    print_memory_map();             // Print memory map information
    log_buffer_print(&frame_log);   // Print frame log
    pci_init();                     // Initialize PCI
    log_buffer_print(&lmodule_log); // Print lmodule log
    init_ide();                     // Initialize ATA/ATAPI driver
    init_serial();                  // Initialize the serial port
    init_parallel();                // Initialize the parallel port
    init_ps2();                     // Initialize PS/2 controller
    init_vfs();                     // Initialize VFS
    tmpfs_regist();                 // Register tmpfs
    init_cpio();                    // Initialize CPIO
    enable_intr();

    panic("No operation.");
}
