/*
 *
 *      main.c
 *      Uinxed-kernel entry
 *
 *      2024/6/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/cpuid.h>
#include <arch/fpu.h>
#include <arch/gdt.h>
#include <arch/smbios.h>
#include <arch/smp.h>
#include <boot/limine_module.h>
#include <cgroup/cgroup.h>
#include <drivers/base/device.h>
#include <drivers/block/ata/pata/ide.h>
#include <drivers/block/ata/sata/ahci.h>
#include <drivers/block/core/gendisk.h>
#include <drivers/block/nvme/nvme.h>
#include <drivers/bus/pci.h>
#include <drivers/char/chrdev.h>
#include <drivers/char/tpm/tpm.h>
#include <drivers/firmware/acpi.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/virtio/virtgpu_drv.h>
#include <drivers/gpu/fbdev/fbcon.h>
#include <drivers/gpu/fbdev/klogo.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/input/ps2/ps2.h>
#include <drivers/net/ethernet/intel/e1000.h>
#include <drivers/net/ethernet/realtek/rtl8139.h>
#include <drivers/net/ethernet/realtek/rtl8169.h>
#include <drivers/parport/parport.h>
#include <drivers/sound/intel/hda.h>
#include <drivers/sound/soundblaster/sb16.h>
#include <drivers/time/rtc.h>
#include <drivers/time/tsc.h>
#include <drivers/tty/serial/8250.h>
#include <drivers/tty/tty.h>
#include <drivers/tty/tty_driver.h>
#include <drivers/usb/core/usb.h>
#include <drivers/usb/host/host.h>
#include <fs/cgroup/cgroupfs.h>
#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <fs/cpio/cpio.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/extfs/extfs.h>
#include <fs/fatfs/fatfs_vfs.h>
#include <fs/isofs/isofs.h>
#include <fs/ntfs/ntfs_vfs.h>
#include <fs/proc/procfs.h>
#include <fs/sysfs/block_sysfs.h>
#include <fs/sysfs/dmi_sysfs.h>
#include <fs/sysfs/fb_sysfs.h>
#include <fs/sysfs/i2c_sysfs.h>
#include <fs/sysfs/input_sysfs.h>
#include <fs/sysfs/kernel_sysfs.h>
#include <fs/sysfs/mem_sysfs.h>
#include <fs/sysfs/net_sysfs.h>
#include <fs/sysfs/pci_sysfs.h>
#include <fs/sysfs/rtc_sysfs.h>
#include <fs/sysfs/sound_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/tpm_sysfs.h>
#include <fs/sysfs/tty_sysfs.h>
#include <fs/tmpfs/tmpfs.h>
#include <ipc/epoll.h>
#include <ipc/futex.h>
#include <ipc/pipe.h>
#include <ipc/posix_mq.h>
#include <ipc/sysv_ipc.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/debug/debug.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/module/module.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/swap.h>
#include <net/core/netdev.h>
#include <net/ipv4/dhcp.h>
#include <net/netlink/netlink.h>
#include <net/socket.h>
#include <process/boot_process.h>
#include <process/elf_loader.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/sched_test.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/eventfd.h>
#include <syscall/memfd.h>
#include <syscall/mmap.h>
#include <syscall/signalfd.h>
#include <syscall/syscall.h>
#include <syscall/timerfd.h>

/* Create init process */
static void swapper_run_init(void)
{
    process_t *init = process_create("init", NULL, NULL);
    if (!init) panic("Failed to create init process.");
    if (!init->task || init->task->pid != 1) panic("User init did not receive PID 1.");

    /*
     * PID 1 starts with full system credentials.  Login/session services are
     * responsible for dropping to the configured desktop user later.
     */
    init->uid      = 0;
    init->gid      = 0;
    init->fsuid    = 0;
    init->fsgid    = 0;
    init_process   = init;
    pid_t init_sid = 0;
    if (process_setsid(init, &init_sid) || init_sid != 1 || init->pgid != 1) panic("Failed to establish init session.");

    static const char *const init_paths[] = {"/sbin/init", "/etc/init", "/bin/init", "/bin/sh"};
    const char              *chosen_path  = NULL;

    for (size_t i = 0; i < sizeof(init_paths) / sizeof(init_paths[0]); i++) {
        char *init_argv[] = {(char *)init_paths[i], NULL};
        if (!elf_loader_load_initial_path(init, init_paths[i], init_argv, NULL)) {
            chosen_path = init_paths[i];
            break;
        }
    }
    if (!chosen_path) panic("No working init found.");

    strncpy(init->exe_path, chosen_path, sizeof(init->exe_path) - 1);
    init->exe_path[sizeof(init->exe_path) - 1] = '\0';

    spin_lock(&scheduler.lock);
    enqueue_task(init->task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(init->task);

    for (uint32_t i = 0; i < sched_cpu_count(); i++)
        if (cpu_rqs[i].idle) cpu_rqs[i].idle->process = init;
    plogk("swapper/0: Init process (pid=1) ready: %s\n", chosen_path);

    /*
     * Kernel init is complete: hand the full screen back to the console.
     * The boot logo is not cleared or redrawn - it stays on screen and the
     * first console scrolls naturally cover it line by line (Linux fbcon
     * behaviour).
     */
    fbcon_release_logo();
    video_start_refresh_worker();
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
    fpu_init();             // Floating-Point Unit / Streaming SIMD Extensions
                            //
    /* Memory Management */ //
    init_frame();           // Physical Memory Frame
    page_init();            // Standard 4-Level Page Table
    init_heap();            // Standard Memory Heap
#if CONFIG_SWAP             //
    swap_init();            // Anonymous-memory swap area manager
#endif                      //
    lmodule_init();         // Limine Kernel Module
                            //
    init_serial();          // Standard RS-232 Serial Port (needs the heap)
    vt_console_init();      // Register vt/drm console drivers (before first printk)
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
    init_gdt();                      // Global Descriptor Table
    init_idt();                      // Interrupt Descriptor Table
    isr_registe_handle();            //
    serial_irq_install();            // Serial IRQ handlers (must follow init_idt)
                                     //
    /* Platform Discovery */         //
    acpi_init();                     // Advanced Configuration and Power Interface
    tpm_init();                      // Trusted Platform Module
    tsc_init();                      // Time Stamp Counter
    smp_init();                      // Symmetric Multiprocessing
    parport_pc_init();               // PC Parallel Port (SPP)
                                     //
    print_memory_map();              //
    log_buffer_print(&frame_log);    //
                                     //
    /* Hardware Bus & Input */       //
    pci_init();                      // Peripheral Component Interconnect
    init_ps2();                      // PS/2 Controller
                                     //
    log_buffer_print(&serial_log);   //
    log_buffer_print(&parallel_log); //
    log_buffer_print(&lmodule_log);  //
                                     //
    /* Device Drivers */             //
#if CONFIG_ATA                       //
    init_ide();                      // ATA / ATAPI
    init_ahci();                     // Advanced Host Controller Interface
#endif                               //
#if CONFIG_NVME                      //
    nvme_init();                     // Non-Volatile Memory Express
#endif                               //
    block_register_all_disks();      // Publish discovered disks into the gendisk registry
    net_init();                      // Initialize ARP/NDP caches and DHCP client
    e1000_init();                    // Intel 8254x Gigabit Ethernet
    rtl8169_init();                  // Realtek RTL8169 Gigabit Ethernet
    rtl8139_init();                  // Realtek RTL8139 Fast Ethernet
    usb_host_pci_scan();             // Discover and init all USB host controllers
    sb16_init();                     // Sound Blaster 16
    hda_init();                      // Intel HD Audio
                                     //
    /* Virtual Filesystem */         //
    init_vfs();                      // Virtual Filesystem
    tmpfs_regist();                  // Temporary File System
    procfs_regist();                 // Process File System
    sysfs_regist();                  // Register sysfs with the VFS layer
    cgroupfs_regist();               // Unified Control Group File System

    if (!get_rootdir()->fsid && vfs_mount(0, get_rootdir()) != EOK) plogk("init: Cannot mount tmpfs to root_dir.\n");

    /* Device Model */
    sysfs_init();                                                  // Create sysfs root kobject and top-level directories
    module_subsystem_init();                                       // Loadable kernel module registry and /sys/module
    device_model_init();                                           // Initialise the device model (bus/class/device)
    ppdev_init();                                                  // /dev/parportN character devices
    chrdev_init();                                                 // Register static character devices
    devtmpfs_init();                                               // Device Temporary File System
    vt_driver_init();                                              // Register vt/aux tty drivers
    tty_devices_populate();                                        // Create /dev/tty*, /dev/ttyS*, /dev/console
                                                                   //
    /* RAM Filesystem */                                           //
    init_cpio();                                                   // Copy In, Copy Out
                                                                   //
    /* Sysfs Population */                                         //
    kernel_sysfs_init();                                           // /sys/kernel/{version,cmdline,hostname,...}
    pci_sysfs_init();                                              // /sys/bus/pci/ + /sys/devices/pci*
    input_sysfs_init();                                            // /sys/class/input/eventX
    block_sysfs_init();                                            // /sys/block/{hdX,sdX,nvme*}
    tty_sysfs_init();                                              // /sys/class/tty/
    net_sysfs_init();                                              // /sys/class/net/<interface>/
    fb_sysfs_init();                                               // /sys/class/graphics/fb0 + platform topology
    mem_sysfs_init();                                              // /sys/class/mem/ (null, zero, full, random, urandom)
    sound_sysfs_init();                                            // /sys/class/sound/cardN + ALSA node sub-devices
    tpm_vfs_init();                                                // /dev/tpm0, /dev/tpmrm0
    tpm_sysfs_init();                                              // /sys/class/tpm{,rm}
    rtc_sysfs_init();                                              // /sys/class/rtc/rtc0
    i2c_sysfs_init();                                              // /sys/bus/i2c + /sys/class/i2c-dev
    dmi_sysfs_init();                                              // /sys/class/dmi/id + /sys/firmware/dmi/tables
                                                                   //
    /* Filesystem Drivers */                                       //
    fatfs_vfs_regist();                                            // FAT File System
    isofs_regist();                                                // ISO 9660 File System
    ntfs_vfs_regist();                                             // New Technology File System
    extfs_regist();                                                // ext2/ext3/ext4 File System
                                                                   //
    /* Process Management */                                       //
    sched_init();                                                  // Preemptive Scheduler
    timer_realtime_set_ns(rtc_since_epoch() * TIMER_NSEC_PER_SEC); // Set realtime clock to current RTC time
    process_init();                                                // Process Management
    signal_init();                                                 // POSIX Signals
    cgroup_init();                                                 // Unified cgroup hierarchy and pids controller
    syscall_init();                                                // Standard System Call
                                                                   //
    /* IPC & Event Notification */                                 //
    pipe_init();                                                   // Pipes
    epoll_init();                                                  // Epoll
    eventfd_init();                                                // Event File Descriptor
    timerfd_init();                                                // Timer File Descriptor
    signalfd_init();                                               // Signal File Descriptor
    inotify_init();                                                // Filesystem Event Notification
    memfd_init();                                                  // Anonymous Memory File Descriptor
                                                                   //
    sysv_ipc_init();                                               // System V IPC
    posix_mq_init();                                               // POSIX Message Queues
    futex_init();                                                  // Futexes
                                                                   //
    netlink_init();                                                // AF_NETLINK socket family (uevent delivery)
    socket_init();                                                 // UNIX Domain Sockets
                                                                   //
    /* Graphics Stack */                                           // Initialise before /dev/fb0 snapshots its size
    drm_init();                                                    // DRM core services
    if (virtio_gpu_init() != 0)                                    // Prefer VirtIO-GPU for card0/renderD128
        drm_init_fallback();                                       // Software fallback only without VirtIO-GPU

    boot_start_init_before_debug(swapper_run_init, sched_test_init);
    e1000_start_workers();
    rtl8169_start_workers();
    rtl8139_start_workers();
    usb_host_start_workers();

    enable_intr();
    sched_start();
}
