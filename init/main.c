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
#include <cgroup/cgroup.h>
#include <chipset/common.h>
#include <chipset/smbios.h>
#include <drivers/acpi.h>
#include <drivers/ahci.h>
#include <drivers/drm/drm_init.h>
#include <drivers/e1000.h>
#include <drivers/hda.h>
#include <drivers/ide.h>
#include <drivers/input_sysfs.h>
#include <drivers/net_sysfs.h>
#include <drivers/nvme.h>
#include <drivers/parallel.h>
#include <drivers/pci.h>
#include <drivers/ps2.h>
#include <drivers/pty.h>
#include <drivers/sb16.h>
#include <drivers/serial.h>
#include <drivers/tpm.h>
#include <drivers/tsc.h>
#include <drivers/tty.h>
#include <drivers/usb.h>
#include <drivers/virt/gpu/virtgpu_drv.h>
#include <fs/cgroupfs.h>
#include <fs/cpio.h>
#include <fs/devtmpfs.h>
#include <fs/fatfs/fatfs_vfs.h>
#include <fs/isofs/isofs.h>
#include <fs/inotify.h>
#include <fs/ntfs/ntfs_vfs.h>
#include <fs/procfs.h>
#include <fs/sysfs.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <ipc/epoll.h>
#include <ipc/futex.h>
#include <ipc/netlink.h>
#include <ipc/posix_mq.h>
#include <ipc/socket.h>
#include <ipc/sysv_ipc.h>
#include <kernel/cmdline.h>
#include <kernel/debug.h>
#include <kernel/device.h>
#include <kernel/elf_loader.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/module.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/swap.h>
#include <net/dhcp.h>
#include <net/netdev.h>
#include <proc/boot_process.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/sched_test.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/eventfd.h>
#include <syscall/memfd.h>
#include <syscall/mmap.h>
#include <syscall/signalfd.h>
#include <syscall/syscall.h>
#include <syscall/timerfd.h>
#include <video/klogo.h>
#include <video/video.h>

extern process_t *init_process;
extern void       pipe_init(void);
extern void       ksysfs_init(void);
extern void       pci_sysfs_init(void);
extern void       block_sysfs_init(void);
extern void       tty_sysfs_init(void);

/* Create init process */
void swapper_run_init(void)
{
    lmodule_t *init_mod = get_lmodule("init");
    if (!init_mod || !init_mod->data || init_mod->size == 0) panic("No working init found.");
    plogk("swapper/0: Found init module at %p, size %zu bytes.\n", init_mod->data, init_mod->size);

    process_t *init = process_create("init", NULL, NULL);
    if (!init) panic("Failed to create init process.");
    if (!init->task || init->task->pid != 1) panic("User init did not receive PID 1.");
    init_process   = init;
    pid_t init_sid = 0;
    if (process_setsid(init, &init_sid) || init_sid != 1 || init->pgid != 1) { panic("Failed to establish init session."); }

    char *init_argv[] = {"/init", NULL};
    if (elf_loader_load_initial_process(init, init_mod->data, init_mod->size, init_argv, NULL)) panic("Failed to load init ELF!");

    spin_lock(&scheduler.lock);
    enqueue_task(init->task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(init->task);

    for (uint32_t i = 0; i < sched_cpu_count(); i++) {
        if (cpu_rqs[i].idle) cpu_rqs[i].idle->process = init;
    }
    plogk("swapper/0: Init process (pid=1) ready.\n");
}

/* Executable entry */
void executable_entry(void)
{
    const char *msg = "Theoretically you should use Limine to boot this kernel, not execute it directly.\n";

    size_t msg_len = 0;
    while (msg[msg_len]) msg_len++;

    __asm__ volatile("mov $1, %%rax\n\t"
                     "mov $1, %%rdi\n\t"
                     "mov %1, %%rsi\n\t"
                     "mov %2, %%rdx\n\t"
                     "syscall\n\t"
                     "mov $60, %%rax\n\t"
                     "mov $1, %%rdi\n\t"
                     "syscall\n\t"
                     :
                     : "r"(msg), "r"(msg), "r"(msg_len)
                     : "%rax", "%rdi", "%rsi", "%rdx", "memory");

    while (1) __asm__ volatile("cli; hlt");
}

/* Kernel entry */
void kernel_entry(void)
{
    /* CPU Features */
    init_fpu();             // Floating-Point Unit / Streaming SIMD Extensions
    init_sse();             // Streaming SIMD Extensions / 2
    init_avx();             // Advanced Vector Extensions / 2
                            //
    /* Early Platform */    //
    init_serial();          // Standard RS-232 Serial Port
                            //
    /* Memory Management */ //
    init_frame();           // Physical Memory Frame
    page_init();            // Standard 4-Level Page Table
    init_heap();            // Standard Memory Heap
    swap_init();            // Anonymous-memory swap area manager
    lmodule_init();         // Limine Kernel Module
                            //
    /* Early Graphics */    //
    video_init();           // Basic VESA/GOP Video
    video_info_t fbinfo = video_get_info();
    video_show_boot_logo();

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
    plogk("cpu: NX (Execute Disable) protection = %s\n", cpu_nx_enabled() ? "active" : "passive");
    plogk("page: kernel_page_dir = %p\n", get_kernel_pagedir());
    plogk("page: kernel_page_table = %p\n", phys_to_virt(get_cr3()));
    plogk("heap: Range: %p - %p (%llu KiB)\n", KERNEL_HEAP_START, KERNEL_HEAP_START + KERNEL_HEAP_SIZE, KERNEL_HEAP_SIZE / 1024);
    plogk("x86/PAT: Configuration [0-7]: %s\n", get_pat_config().pat_str);
    plogk("dmi: %s %s, BIOS %s %s\n", smbios_sys_manufacturer(), smbios_sys_product_name(), smbios_bios_version(), smbios_bios_release_date());

    /* Architecture */
    init_gdt();                   // Global Descriptor Table
    init_idt();                   // Interrupt Descriptor Table
    isr_registe_handle();         //
                                  //
    /* Platform Discovery */      //
    acpi_init();                  // Advanced Configuration and Power Interface
    tpm_init();                   // Trusted Platform Module
    tsc_init();                   // Time Stamp Counter
    smp_init();                   // Symmetric Multiprocessing
    parallel_init();              // IEEE 1284 Parallel Port
                                  //
    print_memory_map();           //
    log_buffer_print(&frame_log); //
                                  //
    /* Hardware Bus & Input */    //
    pci_init();                   // Peripheral Component Interconnect
#if CONFIG_NET
    net_init(); // Network device core, before NIC probes
#endif
    init_ps2();                      // PS/2 Controller
                                     //
    log_buffer_print(&serial_log);   //
    log_buffer_print(&parallel_log); //
    log_buffer_print(&lmodule_log);  //
                                     //
    /* Device Drivers */             //
#if CONFIG_E1000
    e1000_init(); // Intel 8254x Gigabit Ethernet
#endif
#if CONFIG_NET
    dhcp_init(); // Nonblocking IPv4 autoconfiguration
#endif
    sb16_init();             // Sound Blaster 16
    hda_init();              // Intel HD Audio
                             //
    init_ide();              // ATA / ATAPI
    nvme_init();             // Non-Volatile Memory Express
    init_ahci();             // Advanced Host Controller Interface
                             //
    /* Virtual Filesystem */ //
    init_vfs();              // Virtual Filesystem
    tmpfs_regist();          // Temporary File System
    procfs_regist();         // Process File System
    sysfs_regist();          // Register sysfs with the VFS layer
    cgroupfs_regist();       // Unified Control Group File System

    if (!get_rootdir()->fsid && vfs_mount(0, get_rootdir()) != EOK) plogk("init: Cannot mount tmpfs to root_dir.\n");

    /* Device Model */
    sysfs_init();                  // Create sysfs root kobject and top-level directories
    module_subsystem_init();       // Loadable kernel module registry and /sys/module
    device_model_init();           // Initialise the device model (bus/class/device)
    devtmpfs_init();               // Device Temporary File System
                                   //
    /* RAM Filesystem */           //
    init_cpio();                   // Copy In, Copy Out
                                   //
    /* Sysfs Population */         //
    ksysfs_init();                 // /sys/kernel/{version,cmdline,hostname,...}
    pci_sysfs_init();              // /sys/bus/pci/ + /sys/devices/pci*
    input_sysfs_init();            // /sys/class/input/eventX
    block_sysfs_init();            // /sys/block/{hdX,sdX,nvme*}
    tty_sysfs_init();              // /sys/class/tty/
#if CONFIG_USB_XHCI
    xhci_init();                   // USB xHCI host controllers and root devices
#endif
#if CONFIG_NET
    net_sysfs_init();              // /sys/class/net/<interface>/
#endif
                                   //
    /* Filesystem Drivers */       //
    fatfs_vfs_regist();            // FAT File System
    isofs_regist();                // ISO 9660 File System
    ntfs_vfs_regist();             // New Technology File System
                                   //
    /* Terminal Devices */         //
    pty_init();                    // Unix98 pseudo-terminals
                                   //
    /* Process Management */       //
    sched_init();                  // Preemptive Scheduler
    process_init();                // Process Management
    signal_init();                 // POSIX Signals
    cgroup_init();                 // Unified cgroup hierarchy and pids controller
    syscall_init();                // Standard System Call
                                   //
    /* IPC & Event Notification */ //
    pipe_init();                   // Pipes
    epoll_init();                  // Epoll
    eventfd_init();                // Event File Descriptor
    timerfd_init();                // Timer File Descriptor
    signalfd_init();               // Signal File Descriptor
    inotify_init();                // Filesystem Event Notification
    memfd_init();                  // Anonymous Memory File Descriptor
                                   //
    sysv_ipc_init();               // System V IPC
    posix_mq_init();               // POSIX Message Queues
    futex_init();                  // Futexes
                                   //
    netlink_init();                // AF_NETLINK socket family (uevent delivery)
    socket_init();                 // UNIX Domain Sockets
                                   //
    /* Graphics Stack */           //
    drm_init();                    // DRM core services
    if (virtio_gpu_init() != 0)    // Prefer VirtIO-GPU for card0/renderD128
        drm_init_fallback();       // Software fallback only without VirtIO-GPU

    boot_start_init_before_debug(swapper_run_init, sched_test_init);
#if CONFIG_E1000
    e1000_start_workers();
#endif
#if CONFIG_USB_XHCI
    xhci_start_workers();
#endif

    enable_intr();
    sched_start();
}
