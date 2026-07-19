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
#include <errno.h>
#include <eis.h>
#include <frame.h>
#include <gdt.h>
#include <heap.h>
#include <hhdm.h>
#include <ide.h>
#include <interrupt.h>
#include <limine_module.h>
#include <page.h>
#include <parallel.h>
#include <pci.h>
#include <printk.h>
#include <ps2.h>
#include <sched.h>
#include <serial.h>
#include <smbios.h>
#include <smp.h>
#include <fatfs_vfs.h>
#include <tmpfs.h>
#include <tsc.h>
#include <uinxed.h>
#include <vfs.h>
#include <video.h>
#include <devtmpfs.h>

static volatile uint64_t preempt_demo_sink;
static wait_queue_t      demo_wait_queue;
static wait_queue_t      migration_wait_queue;
static task_t           *migration_task;

static void scheduler_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    for (uint64_t i = 0; i < 8; i++) {
        plogk("sched: %s iteration %llu on task %llu cpu %u\n", name, i, current_task()->pid, current_task()->cpu_id);
        task_sleep_ticks(2);
    }
}

static void preempt_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    plogk("sched: %s busy loop start on task %llu cpu %u\n", name, current_task()->pid, current_task()->cpu_id);
    for (uint64_t chunk = 0; chunk < 3; chunk++) {
        for (uint64_t i = 0; i < 5000000; i++) preempt_demo_sink += i;
        plogk("sched: %s busy chunk %llu cpu %u\n", name, chunk, current_task()->cpu_id);
    }
    plogk("sched: %s busy loop done\n", name);
}

static void wait_demo_thread(void *arg)
{
    const char *name = (const char *)arg;

    plogk("sched: %s waiting at tick %llu\n", name, sched_ticks());
    wait_queue_wait(&demo_wait_queue);
    plogk("sched: %s woke at tick %llu on task %llu cpu %u\n", name, sched_ticks(), current_task()->pid, current_task()->cpu_id);
}

static void wake_demo_thread(void *arg)
{
    (void)arg;

    task_sleep_ticks(8);
    task_t *task = wait_queue_wake_one(&demo_wait_queue);
    plogk("sched: wait queue wake_one target task %llu\n", task ? task->pid : 0);
}

static void keyboard_wait_thread(void *arg)
{
    (void)arg;

    plogk("init: Keyboard waiter blocking for an input event.\n");
    ps2kbd_wait_events();
    plogk("init: Keyboard waiter received an input event.\n");
}

static void migration_wait_thread(void *arg)
{
    (void)arg;

    plogk("sched: migration waiter started on task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
    wait_queue_wait(&migration_wait_queue);
    plogk("sched: migration waiter woke on task %llu cpu %u\n", current_task()->pid, current_task()->cpu_id);
}

static void migration_wake_thread(void *arg)
{
    (void)arg;

    task_sleep_ticks(12);
    if (migration_task && sched_cpu_count() > 1) {
        int status = task_set_cpu(migration_task, 1);
        plogk("sched: migration target task %llu to cpu 1 status %d\n", migration_task->pid, status);
    }
    task_t *task = wait_queue_wake_one(&migration_wait_queue);
    plogk("sched: migration wake target task %llu\n", task ? task->pid : 0);
}

static void kernel_init_thread(void *arg)
{
    (void)arg;

    plogk("init: Kernel init thread started as task %llu cpu %u.\n", current_task()->pid, current_task()->cpu_id);
    wait_queue_init(&demo_wait_queue);
    wait_queue_init(&migration_wait_queue);
    kthread_create("preempt-demo", preempt_demo_thread, "preempt-demo");
    kthread_create("demo-a", scheduler_demo_thread, "demo-a");
    kthread_create("demo-b", scheduler_demo_thread, "demo-b");
    kthread_create("wait-demo", wait_demo_thread, "wait-demo");
    kthread_create("wake-demo", wake_demo_thread, NULL);
    kthread_create("keyboard-wait", keyboard_wait_thread, NULL);
    migration_task = kthread_create_on_cpu("migration-wait", migration_wait_thread, NULL, 0);
    kthread_create("migration-wake", migration_wake_thread, NULL);

    while (1) task_sleep_ticks(250);
}

/* Executable entry */
void executable_entry(void)
{
    disable_intr();
    while (1) __asm__ volatile("hlt");
}

/* Kernel entry */
void kernel_entry(void)
{
    init_fpu(); // Initialize FPU/MMX
    init_sse(); // Initialize SSE/SSE2
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
    init_avx();                     // Initialize AVX/AVX2
    acpi_init();                    // Initialize ACPI
    tsc_init();                     // Initialize TSC
    smp_init();                     // Initialize SMP
    print_memory_map();             // Print memory map information
    log_buffer_print(&frame_log);   // Print frame log
    pci_init();                     // Initialize PCI
    log_buffer_print(&lmodule_log); // Print lmodule log
    init_ide();                     // Initialize ATA/ATAPI driver
    init_parallel();                // Initialize the parallel port
    init_ps2();                     // Initialize PS/2 controller
    init_vfs();                     // Initialize VFS
    tmpfs_regist();                 // Register tmpfs
    fatfs_vfs_regist();             // Register FatFs VFS bridge
    if (!get_rootdir()->fsid && vfs_mount(0, get_rootdir()) != EOK)
        plogk("init: Cannot mount tmpfs to root_dir.\n");
    init_cpio();                    // Initialize CPIO
    devtmpfs_init();
    sched_init();
    kthread_create("kernel-init", kernel_init_thread, NULL);
    enable_intr();

    sched_start();
}
