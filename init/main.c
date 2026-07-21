/*
 *
 *      main.c
 *      Uinxed-kernel entry
 *
 *      2024/6/23 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <acpi.h>
#include <cmdline.h>
#include <common.h>
#include <cpio.h>
#include <cpuid.h>
#include <debug.h>
#include <elf_loader.h>
#include <devtmpfs.h>
#include <eis.h>
#include <errno.h>
#include <eventfd.h>
#include <fatfs_vfs.h>
#include <frame.h>
#include <gdt.h>
#include <heap.h>
#include <hhdm.h>
#include <ide.h>
#include <interrupt.h>
#include <limine_module.h>
#include <mmap.h>
#include <page.h>
#include <parallel.h>
#include <pci.h>
#include <printk.h>
#include <process.h>
#include <procfs.h>
#include <ps2.h>
#include <sched.h>
#include <sched_test.h>
#include <serial.h>
#include <signalfd.h>
#include <smbios.h>
#include <smp.h>
#include <sound/sb16.h>
#include <syscall.h>
#include <timerfd.h>
#include <tmpfs.h>
#include <tsc.h>
#include <uinxed.h>
#include <vfs.h>
#include <video.h>

extern process_t *init_process;

void user_init_process(void *arg)
{
    (void)arg;

    plogk("init: user_init_process started (pid=1).\n");

    lmodule_t *init_mod = get_lmodule("init");
    if (!init_mod || !init_mod->data || init_mod->size == 0) {
        plogk("init: warning - init module not found.\n");
        sched_dequeue_current();
        goto halt;
    }

    plogk("init: Found init module at %p, size %zu bytes.\n", init_mod->data, init_mod->size);

    if (elf_loader_load_user_process(init_process, init_mod->data, init_mod->size)) {
        plogk("init: Failed to load init ELF.\n");
        sched_dequeue_current();
        goto halt;
    }

    plogk("init: ELF loaded, returning to scheduler for user mode entry.\n");
    return;

halt:
    while (1) {
        enable_intr();
        __asm__ volatile("hlt");
        disable_intr();
        sched_yield();
    }
}

/* Executable entry */
void executable_entry(void)
{
    const char *msg = "Theoretically you should use Limine to boot this kernel, not execute it directly.\n";

    __asm__ volatile (
        "mov $1, %%rax\n\t"
        "mov $1, %%rdi\n\t"
        "mov %1, %%rsi\n\t"
        "mov %2, %%rdx\n\t"
        "syscall\n\t"
        "mov $60, %%rax\n\t"
        "mov $1, %%rdi\n\t"
        "syscall\n\t"
        :
        : "r"(msg), "r"(msg), "i"(83)
        : "%rax", "%rdi", "%rsi", "%rdx", "memory"
    );

    while (1) __asm__ volatile ("cli; hlt");
}

/* Kernel entry */
void kernel_entry(void)
{
    init_fpu();    // Initialize FPU/MMX
    init_sse();    // Initialize SSE/SSE2
    init_serial(); // Initialize the serial port

    init_frame();   // Initialize memory frame
    page_init();    // Initialize memory page
    init_heap();    // Initialize the memory heap
    lmodule_init(); // Initialize the passed-in resource module list

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
    syscall_init();                 // Register syscall entry
    init_avx();                     // Initialize AVX/AVX2
    acpi_init();                    // Initialize ACPI
    tsc_init();                     // Initialize TSC
    smp_init();                     // Initialize SMP
    print_memory_map();             // Print memory map information
    log_buffer_print(&frame_log);   // Print frame log
    pci_init();                     // Initialize PCI
    sb16_init();                    // Initialize SB16 sound card
    log_buffer_print(&lmodule_log); // Print lmodule log
    init_ide();                     // Initialize ATA/ATAPI driver
    init_parallel();                // Initialize the parallel port
    init_ps2();                     // Initialize PS/2 controller
    init_vfs();                     // Initialize VFS
    tmpfs_regist();                 // Register tmpfs
    fatfs_vfs_regist();             // Register FatFs VFS bridge
    if (!get_rootdir()->fsid && vfs_mount(0, get_rootdir()) != EOK) plogk("init: Cannot mount tmpfs to root_dir.\n");
    init_cpio(); // Initialize CPIO
    devtmpfs_init();
    procfs_regist();
    {
        vfs_node_t proc = 0;
        int        st   = vfs_mkdir("/proc");
        if (st == EOK || st == -EEXIST) {
            proc = vfs_open("/proc");
            if (proc) {
                st = vfs_mount_fs("procfs", NULL, proc);
                if (st == EOK) {
                    plogk("procfs: Mounted at /proc.\n");
                    vfs_close(proc);
                } else {
                    plogk("procfs: Cannot mount at /proc: %d\n", st);
                    vfs_close(proc);
                }
            }
        }
    }
    sched_init();
    process_init();
    eventfd_init();
    timerfd_init();
    signalfd_init();
    mmap_init();
    sched_test_init();
    enable_intr();

    sched_start();

    panic("Reached an unreachable kernel code area!");
}
