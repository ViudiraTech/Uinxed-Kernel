/*
 *
 *      main.c
 *      Uinxed-kernel entry
 *
 *      2024/6/23 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/cpuid.h>
#include <arch/eis.h>
#include <arch/gdt.h>
#include <arch/smp.h>
#include <boot/limine_module.h>
#include <chipset/common.h>
#include <chipset/smbios.h>
#include <drivers/acpi.h>
#include <drivers/drm/drm_init.h>
#include <drivers/ide.h>
#include <drivers/parallel.h>
#include <drivers/pci.h>
#include <drivers/ps2.h>
#include <drivers/sb16.h>
#include <drivers/serial.h>
#include <drivers/tsc.h>
#include <fs/cpio.h>
#include <fs/devtmpfs.h>
#include <fs/fatfs/fatfs_vfs.h>
#include <fs/procfs.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <kernel/cmdline.h>
#include <kernel/debug.h>
#include <kernel/elf_loader.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/sched_test.h>
#include <syscall/eventfd.h>
#include <syscall/mmap.h>
#include <syscall/signalfd.h>
#include <syscall/syscall.h>
#include <syscall/timerfd.h>
#include <video/klogo.h>
#include <video/video.h>

extern process_t *init_process;

void user_init_process(void *arg)
{
    (void)arg;

    plogk("init: user_init_process started (pid=1)\n");

    lmodule_t *init_mod = get_lmodule("init");
    if (!init_mod || !init_mod->data || init_mod->size == 0) {
        plogk("init: warning - init module not found.\n");
        sched_dequeue_current();
        goto halt;
    }

    plogk("init: Found init module at %p, size %zu bytes.\n", init_mod->data, init_mod->size);

    if (elf_loader_load_user_process(init_process, init_mod->data, init_mod->size, NULL, NULL)) {
        plogk("init: Failed to load init ELF.\n");
        sched_dequeue_current();
        goto halt;
    }

    plogk("init: ELF loaded, returning to scheduler for user mode entry.\n");
    return;

halt:
    process_exit(1);
}

/* Executable entry */
void executable_entry(void)
{
    const char *msg = "Theoretically you should use Limine to boot this kernel, not execute it directly.\n";

    __asm__ volatile("mov $1, %%rax\n\t"
                     "mov $1, %%rdi\n\t"
                     "mov %1, %%rsi\n\t"
                     "mov %2, %%rdx\n\t"
                     "syscall\n\t"
                     "mov $60, %%rax\n\t"
                     "mov $1, %%rdi\n\t"
                     "syscall\n\t"
                     :
                     : "r"(msg), "r"(msg), "i"(83)
                     : "%rax", "%rdi", "%rsi", "%rdx", "memory");

    while (1) __asm__ volatile("cli; hlt");
}

/* Kernel entry */
void kernel_entry(void)
{
    init_fpu();    // Floating-Point Unit / Streaming SIMD Extensions
    init_sse();    // Streaming SIMD Extensions / 2
    init_serial(); // Standard RS-232 Serial Port

    init_frame();   // Physical Memory Frame
    page_init();    // Standard 4-Level Page Table
    init_heap();    // Standard Memory Heap
    lmodule_init(); // Limine Kernel Module

    video_init(); // Basic VESA/GOP Video
    video_info_t fbinfo = video_get_info();

#if BOOT_LOGO
    struct limine_smp_response *smp = smp_request.response;
    video_draw_logo((!CPU_MAX_COUNT) ? smp->cpu_count : (smp->cpu_count > CPU_MAX_COUNT ? CPU_MAX_COUNT : smp->cpu_count));
#endif

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

    init_gdt(); // Global Descriptor Table
    init_idt(); // Interrupt Descriptor Table
    isr_registe_handle();
    syscall_init(); // Standard System Call
    init_avx();     // Advanced Vector Extensions / 2
    acpi_init();    // Advanced Configuration and Power Interface
    tsc_init();     // Time Stamp Counter
    smp_init();     // Symmetric Multiprocessing
    print_memory_map();
    log_buffer_print(&frame_log);
    pci_init();  // Peripheral Component Interconnect
    sb16_init(); // Sound Blaster 16
    log_buffer_print(&lmodule_log);
    init_ide();         // Advanced Technology Attachment / ATA Packet Interface
    init_parallel();    // Standard IEEE 1284 Parallel Port
    init_ps2();         // Personal System/2 Controller
    init_vfs();         // Virtual Filesystem
    tmpfs_regist();     // Temporary File System
    fatfs_vfs_regist(); // FAT File System

    if (!get_rootdir()->fsid && vfs_mount(0, get_rootdir()) != EOK) plogk("init: Cannot mount tmpfs to root_dir.\n");

    init_cpio();     // Copy In, Copy Out
    devtmpfs_init(); // Device Temporary File System
    procfs_regist(); // Process File System

    drm_init(); // Direct Rendering Manager
//  drm_run_test();

    {
        vfs_node_t proc = 0;
        int        st   = vfs_mkdir("/proc");
        if (st == EOK || st == -EEXIST) {
            proc = vfs_open("/proc");
            if (proc) {
                st = vfs_mount_fs("procfs", NULL, proc);
                if (st == EOK) {
                    plogk("procfs: Mounted at /proc\n");
                    vfs_close(proc);
                } else {
                    plogk("procfs: Cannot mount at /proc: %d\n", st);
                    vfs_close(proc);
                }
            }
        }
    }

    sched_init();    // Preemptive Scheduler
    process_init();  // Process Management
    eventfd_init();  // Event File Descriptor
    timerfd_init();  // Timer File Descriptor
    signalfd_init(); // Signal File Descriptor
    mmap_init();     // Memory Map
    sched_test_init();

    enable_intr();
    sched_start();

    panic("Reached an unreachable kernel code area!");
}
