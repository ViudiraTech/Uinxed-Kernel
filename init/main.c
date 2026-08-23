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
#include <drivers/firmware/apic.h>
#include <drivers/gpu/fbdev/fbcon.h>
#include <drivers/gpu/fbdev/klogo.h>
#include <drivers/gpu/fbdev/video.h>
#include <drivers/gpu/gpu_drivers.h>
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
#include <fs/sysfs/drm_sysfs.h>
#include <fs/sysfs/fb_sysfs.h>
#include <fs/sysfs/i2c_sysfs.h>
#include <fs/sysfs/input_sysfs.h>
#include <fs/sysfs/kernel_sysfs.h>
#include <fs/sysfs/mem_sysfs.h>
#include <fs/sysfs/module_sysfs.h>
#include <fs/sysfs/net_sysfs.h>
#include <fs/sysfs/pci_sysfs.h>
#include <fs/sysfs/rtc_sysfs.h>
#include <fs/sysfs/sound_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/tpm_sysfs.h>
#include <fs/sysfs/tty_sysfs.h>
#include <fs/sysfs/usb_sysfs.h>
#include <fs/tmpfs/tmpfs.h>
#include <ipc/epoll.h>
#include <ipc/futex.h>
#include <ipc/pipe.h>
#include <ipc/posix_mq.h>
#include <ipc/sysv_ipc.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/config.h>
#include <kernel/debug/debug.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/module/module.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/swap.h>
#include <net/core/netdev.h>
#include <net/ipv4/dhcp.h>
#include <net/netlink/netlink.h>
#include <net/socket.h>
#include <process/elf_loader.h>
#include <process/process.h>
#include <process/sched.h>
#include <security/seccomp.h>
#include <sync/signal.h>
#include <sync/spin_lock.h>
#include <syscall/eventfd.h>
#include <syscall/fcntl.h>
#include <syscall/memfd.h>
#include <syscall/mmap.h>
#include <syscall/signalfd.h>
#include <syscall/syscall.h>
#include <syscall/timerfd.h>

/* Executable entry */
void executable_entry(void)
{
    const char *msg     = "Theoretically you should use Limine to boot this kernel, not execute it directly.\n";
    size_t      msg_len = 0;
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

/* Load `path` as PID 1, logging any failure and returning the status. */
static int swapper_try_init_path(process_t *init, const char *path)
{
    char *init_argv[] = {(char *)path, NULL};
    int   status      = elf_loader_load_initial_path(init, path, init_argv, NULL);

    if (!status) return 0;

    plogk(status != -ENOENT ? "swapper/0: %s exists but couldn't execute it (error %d)\n" : "swapper/0: %s not found.\n", path, status);
    return status;
}

/* Create init process */
static void swapper_run_init(void)
{
    process_t *init = process_create("init");
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

    /*
     * Hand PID 1 the console as its standard descriptors before it runs.
     * Mirrors Linux kernel_init_freeable(): a missing console is a warning,
     * never a reason to abandon init.
     */
    {
        vfs_node_t console = vfs_open("/dev/console");
        if (!console) {
            plogk("swapper/0: Unable to open an initial console.\n");
        } else {
            int std_fd = process_fd_install(init, console, O_RDWR | O_NOCTTY);
            if (std_fd != 0) {
                /* Should never happen: a fresh process has fd 0 free. */
                if (std_fd < 0)
                    vfs_close(console);
                else
                    process_fd_close(init, std_fd);
                plogk("swapper/0: Unable to open an initial console.\n");
            } else {
                /*
                 * process_fd_dup2() returns the NEW descriptor number (1 or 2)
                 * on success, like Linux; only a negative value is an error.
                 * Comparing against EOK (0) misreported every successful dup.
                 */
                int dup_stdout = process_fd_dup2(init, 0, 1);
                if (dup_stdout < 0) plogk("init: dup2 stdout failed (err=%d)\n", dup_stdout);
                int dup_stderr = process_fd_dup2(init, 0, 2);
                if (dup_stderr < 0) plogk("init: dup2 stderr failed (err=%d)\n", dup_stderr);
                (void)tty_console_acquire(init, O_RDWR);
            }
        }
    }
    const char *chosen_path = NULL; // raw path stored in exe_path

    /* init= value buffer; chosen_path may point into it. */
    char init_opt[VFS_PATH_MAX];

#if CONFIG_INIT_MODULE
    lmodule_t  *init_mod = get_lmodule("init");
    const char *mod_name = init_mod ? (init_mod->path ? init_mod->path : init_mod->name) : "init";
#endif

    /* Probe order: init=, CONFIG_INIT_PATH, conventional paths, then the bootloader module. */
    const char *init_param = cmdline_get_option("init", init_opt, sizeof(init_opt));
    if (init_param && !swapper_try_init_path(init, init_param)) chosen_path = init_param;
    if (!chosen_path && CONFIG_INIT_PATH[0] && !swapper_try_init_path(init, CONFIG_INIT_PATH)) chosen_path = CONFIG_INIT_PATH;
    if (!chosen_path) {
        static const char *const init_paths[] = {"/sbin/init", "/etc/init", "/bin/init", "/bin/sh"};
        const size_t             n_paths      = sizeof(init_paths) / sizeof(init_paths[0]);
        for (size_t i = 0; i < n_paths; i++) {
            if (!swapper_try_init_path(init, init_paths[i])) {
                chosen_path = init_paths[i];
                break;
            }
        }

#if CONFIG_INIT_MODULE
        /* Bootloader "init" module as a last resort. */
        if (!chosen_path) {
            char *init_argv[] = {(char *)mod_name, NULL};
            int   status      = (init_mod && init_mod->data && init_mod->size) ? elf_loader_load_initial_process(init, init_mod->data, init_mod->size, init_argv, NULL) : -ENOENT;
            if (!status) {
                chosen_path = mod_name;
            } else {
                plogk(status != -ENOENT ? "swapper/0: %s exists but couldn't execute it (error %d)\n" : "swapper/0: %s not found.\n", mod_name, status);
            }
        }
#endif
    }
    if (!chosen_path) panic("No working init found.");
    strncpy(init->exe_path, chosen_path, sizeof(init->exe_path) - 1);
    init->exe_path[sizeof(init->exe_path) - 1] = '\0';

    for (uint32_t i = 0; i < sched_cpu_count(); i++)
        if (cpu_rqs[i].idle) cpu_rqs[i].idle->process = init;
    plogk("swapper/0: Init process (pid=1) ready: %s\n", chosen_path);

    /*
     * Kernel init is complete: hand the full screen back to the console.
     * The boot logo is not cleared or redrawn - it stays on screen and the
     * first console scrolls naturally cover it line by line.
     */
    fbcon_release_logo();
}

/* Make init runnable only after driver init is complete. */
static void swapper_enqueue_init(void)
{
    spin_lock(&scheduler.lock);
    enqueue_task(init_process->task);
    spin_unlock(&scheduler.lock);
    request_task_cpu(init_process->task);
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
    swap_init();            // Anonymous-memory swap area manager
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
    init_gdt();                                                    // Global Descriptor Table
    init_idt();                                                    // Interrupt Descriptor Table
    isr_registe_handle();                                          //
    serial_irq_install();                                          // Serial IRQ handlers (must follow init_idt)
                                                                   //
    /* Platform Discovery */                                       //
    acpi_init();                                                   // Advanced Configuration and Power Interface
    tpm_init();                                                    // Trusted Platform Module
    tsc_init();                                                    // Time Stamp Counter
    lapic_timer_try_upgrade();                                     // Switch BSP LAPIC timer to TSC-deadline now that TSC is calibrated (before APs boot)
    smp_init();                                                    // Symmetric Multiprocessing
    parport_pc_init();                                             // PC Parallel Port (SPP)
                                                                   //
    print_memory_map();                                            //
    log_buffer_print(&frame_log);                                  //
                                                                   //
    /* Hardware Bus & Input */                                     //
    pci_init();                                                    // Peripheral Component Interconnect
    init_ps2();                                                    // PS/2 Controller
    parport_pc_init();                                             // PC Parallel Port (SPP)
                                                                   //
    log_buffer_print(&serial_log);                                 //
    log_buffer_print(&parallel_log);                               //
    log_buffer_print(&lmodule_log);                                //
                                                                   //
    /* Process Management */                                       //
    sched_init();                                                  // Preemptive Scheduler
    timer_realtime_set_ns(rtc_since_epoch() * TIMER_NSEC_PER_SEC); // Set realtime clock to current RTC time
    process_init();                                                // Process Management
    signal_init();                                                 // POSIX Signals
    cgroup_init();                                                 // Unified cgroup hierarchy and pids controller
    syscall_init();                                                // Standard System Call
                                                                   //
    /* Virtual Filesystem */                                       //
    init_vfs();                                                    // Virtual Filesystem
    tmpfs_regist();                                                // Temporary File System
    procfs_regist();                                               // Process File System
    sysfs_regist();                                                // Register sysfs with the VFS layer
    cgroupfs_regist();                                             // Unified Control Group File System

    if (!get_rootdir()->fsid && vfs_mount(0, get_rootdir()) != EOK) plogk("init: Cannot mount tmpfs to root_dir.\n");

    /* Filesystem Drivers */       //
    fatfs_vfs_regist();            // FAT File System
    isofs_regist();                // ISO 9660 File System
    ntfs_vfs_regist();             // New Technology File System
    extfs_regist();                // ext2/ext3/ext4 File System
                                   //
    /* IPC & Event Notification */ //
    pipe_init();                   // Pipes
    pidfd_init();                  // Process file descriptors
    epoll_init();                  // Epoll
    eventfd_init();                // Event File Descriptor
    seccomp_init();                // Secure computing filters and notifications
    timerfd_init();                // Timer File Descriptor
    signalfd_init();               // Signal File Descriptor
    inotify_init();                // Filesystem Event Notification
    memfd_init();                  // Anonymous Memory File Descriptor
    posix_mq_init();               // POSIX Message Queues
    socket_init();                 // UNIX Domain Sockets
                                   //
    sysv_ipc_init();               // System V IPC
    futex_init();                  // Futexes
    netlink_init();                // AF_NETLINK socket family (uevent delivery)
                                   //
    /* Device Model */             //
    sysfs_kobject_init();          // Create sysfs root kobject and top-level directories
    module_subsystem_init();       // Loadable kernel module registry and /sys/module
    device_model_init();           // Initialise the device model (bus/class/device)
    ppdev_init();                  // /dev/parportN character devices
    chrdev_init();                 // Register static character devices
    vt_driver_init();              // Register vt/aux tty drivers
    devtmpfs_init();               // Device Temporary File System
    /* Device Drivers */           //
    init_ide();                    // ATA / ATAPI
    init_ahci();                   // Advanced Host Controller Interface
    nvme_init();                   // Non-Volatile Memory Express
    block_register_all_disks();    // Publish discovered disks into the gendisk registry
    net_init();                    // Initialize ARP/NDP caches and DHCP client
    e1000_init();                  // Intel 8254x Gigabit Ethernet
    rtl8169_init();                // Realtek RTL8169 Gigabit Ethernet
    rtl8139_init();                // Realtek RTL8139 Fast Ethernet
    usb_host_pci_scan();           // Discover and init all USB host controllers
    sb16_init();                   // Sound Blaster 16
    hda_init();                    // Intel HD Audio
                                   //
                                   //
    /* RAM Filesystem */           //
    init_cpio();                   // Copy In, Copy Out
                                   //
    /* Sysfs Population */         //
    kernel_sysfs_init();           // /sys/kernel/{version,cmdline,hostname,...}
    pci_sysfs_init();              // /sys/bus/pci/ + /sys/devices/pci*
    i2c_sysfs_init();              // /sys/bus/i2c + /sys/class/i2c-dev
    usb_sysfs_init();              // /sys/bus/usb/ + /sys/bus/usb/devices/
    block_sysfs_init();            // /sys/block/{hdX,sdX,nvme*}
    tty_sysfs_init();              // /sys/class/tty/
    net_sysfs_init();              // /sys/class/net/<interface>/
    input_sysfs_init();            // /sys/class/input/eventX
    fb_sysfs_init();               // /sys/class/graphics/fb0 + platform topology
    mem_sysfs_init();              // /sys/class/mem/ (null, zero, full, random, urandom)
    sound_sysfs_init();            // /sys/class/sound/cardN + ALSA node sub-devices
    rtc_sysfs_init();              // /sys/class/rtc/rtc0
    tpm_vfs_init();                // /dev/tpm0, /dev/tpmrm0
    tpm_sysfs_init();              // /sys/class/tpm{,rm}
    dmi_sysfs_init();              // /sys/class/dmi/id + /sys/firmware/dmi/tables
    drm_sysfs_init();              // /sys/class/drm/
    module_sysfs_init();           // /sys/module/<name>/
                                   //
    /* Graphics Stack */           // Initialise before /dev/fb0 snapshots its size
    gpu_drivers_init();            // Register every built-in GPU driver
    gpu_drivers_probe();           // Attach a GPU, or a framebuffer fallback

    /*
     * kthreadd must be live before any subsystem creates a kernel worker,
     * while init must remain non-runnable until kernel bootstrap work has
     * completed. kthread_create() may sleep waiting for kthreadd; the
     * bootstrap scheduler can already perform task switches at this stage,
     * allowing kthreadd to service requests before sched_start() is entered.
     */
    swapper_run_init();           // Create init (PID 1), but keep it dormant
    kthreadd_init();              // Create and enqueue kthreadd (PID 2)
                                  //
    e1000_start_workers();        // Register e1000 workers
    rtl8169_start_workers();      // Register rtl8169 workers
    rtl8139_start_workers();      // Register rtl8139 workers
    usb_host_start_workers();     // Register USB host workers
    video_start_refresh_worker(); // Register display refresh worker
    timer_deferred_init();        // Register timer bottom-half processing
    kernel_workers_start();       // Create every registered kernel worker
    swapper_enqueue_init();       // Finally make init runnable
                                  //
    enable_intr();                // Enable interrupts
    sched_start();                // Hand execution over to the scheduler

    /*
     * sched_start() must never return. Reaching this point means the
     * scheduler violated its non-returning contract; do not allow
     * execution to fall through the end of kernel_entry().
     */
    panic("Scheduler returned.");
}
