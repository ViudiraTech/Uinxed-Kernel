/*
 *
 *      config.h
 *      Kernel configuration defaults
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_CONFIG_H_
#define INCLUDE_CONFIG_H_

/*
 * Build-time configuration defaults for every Kconfig option the Makefile can
 * emit.  The Makefile passes the final values with -D...; these #ifndef
 * fallbacks keep every translation unit self-contained when a define is
 * missing, matching the Kconfig default.
 */

#ifndef BOOT_LOGO
#    define BOOT_LOGO 1
#endif

#ifndef CONFIG_ATA
#    define CONFIG_ATA 1
#endif

#ifndef CONFIG_CGROUP
#    define CONFIG_CGROUP 1
#endif

#ifndef CONFIG_DEVTMPFS_MOUNT
#    define CONFIG_DEVTMPFS_MOUNT 1
#endif

#ifndef CONFIG_DRM
#    define CONFIG_DRM 1
#endif

#ifndef CONFIG_E1000
#    define CONFIG_E1000 1
#endif

#ifndef CONFIG_EXTFS
#    define CONFIG_EXTFS 1
#endif

#ifndef CONFIG_FAT_FS
#    define CONFIG_FAT_FS 1
#endif

#ifndef CONFIG_I2C
#    define CONFIG_I2C 1
#endif

#ifndef CONFIG_INET
#    define CONFIG_INET 1
#endif

#ifndef CONFIG_INPUT_EVDEV
#    define CONFIG_INPUT_EVDEV 1
#endif

#ifndef CONFIG_ISO9660_FS
#    define CONFIG_ISO9660_FS 1
#endif

#ifndef CONFIG_MODULE_FORCE_LOAD
#    define CONFIG_MODULE_FORCE_LOAD 0
#endif

#ifndef CONFIG_MODULE_FORCE_UNLOAD
#    define CONFIG_MODULE_FORCE_UNLOAD 0
#endif

#ifndef CONFIG_MODULE_MAX_SIZE
#    define CONFIG_MODULE_MAX_SIZE 64
#endif

#ifndef CONFIG_MODULE_SIG_FORCE
#    define CONFIG_MODULE_SIG_FORCE 0
#endif

#ifndef CONFIG_MODULES
#    define CONFIG_MODULES 1
#endif

#ifndef CONFIG_NET
#    define CONFIG_NET 1
#endif

#ifndef CONFIG_NETLINK
#    define CONFIG_NETLINK 1
#endif

#ifndef CONFIG_NTFS_FS
#    define CONFIG_NTFS_FS 1
#endif

#ifndef CONFIG_NVME
#    define CONFIG_NVME 1
#endif

#ifndef CONFIG_PARPORT
#    define CONFIG_PARPORT 1
#endif

#ifndef CONFIG_PCSPKR
#    define CONFIG_PCSPKR 0
#endif

#ifndef CONFIG_POSIX_MQ
#    define CONFIG_POSIX_MQ 1
#endif

#ifndef CONFIG_PS2_KEYBOARD_MOUSE
#    define CONFIG_PS2_KEYBOARD_MOUSE 1
#endif

#ifndef CONFIG_RTL8139
#    define CONFIG_RTL8139 1
#endif

#ifndef CONFIG_RTL8169
#    define CONFIG_RTL8169 1
#endif

#ifndef CONFIG_SERIAL
#    define CONFIG_SERIAL 1
#endif

#ifndef CONFIG_SOUND_HDA
#    define CONFIG_SOUND_HDA 1
#endif

#ifndef CONFIG_SOUND_SB16
#    define CONFIG_SOUND_SB16 0
#endif

#ifndef CONFIG_SWAP
#    define CONFIG_SWAP 1
#endif

#ifndef CONFIG_SYSFS
#    define CONFIG_SYSFS 1
#endif

#ifndef CONFIG_SYSVIPC
#    define CONFIG_SYSVIPC 1
#endif

#ifndef CONFIG_TPM
#    define CONFIG_TPM 1
#endif

#ifndef CONFIG_UEVENT_HELPER
#    define CONFIG_UEVENT_HELPER 1
#endif

#ifndef CONFIG_UNIX98_PTY_MAX
#    define CONFIG_UNIX98_PTY_MAX 4096
#endif

#ifndef CONFIG_UNIX98_PTYS
#    define CONFIG_UNIX98_PTYS 1
#endif

#ifndef CONFIG_USB
#    define CONFIG_USB 1
#endif

#ifndef CONFIG_USB_EHCI
#    define CONFIG_USB_EHCI 1
#endif

#ifndef CONFIG_USB_HID
#    define CONFIG_USB_HID 1
#endif

#ifndef CONFIG_USB_OHCI
#    define CONFIG_USB_OHCI 0
#endif

#ifndef CONFIG_USB_STORAGE
#    define CONFIG_USB_STORAGE 1
#endif

#ifndef CONFIG_USB_UHCI
#    define CONFIG_USB_UHCI 0
#endif

#ifndef CONFIG_USB_XHCI
#    define CONFIG_USB_XHCI 1
#endif

#ifndef CONFIG_VIRTIO
#    define CONFIG_VIRTIO 0
#endif

#ifndef CONFIG_VIRTIO_GPU
#    define CONFIG_VIRTIO_GPU 0
#endif

#ifndef CONFIG_VT
#    define CONFIG_VT 1
#endif

#ifndef CONFIG_VT_COUNT
#    define CONFIG_VT_COUNT 8
#endif

#ifndef CPU_FEATURE_AVX
#    define CPU_FEATURE_AVX 1
#endif

#ifndef CPU_FEATURE_AVX512
#    define CPU_FEATURE_AVX512 0
#endif

#ifndef CPU_FEATURE_FPU
#    define CPU_FEATURE_FPU 1
#endif

#ifndef CPU_FEATURE_SSE
#    define CPU_FEATURE_SSE 1
#endif

#ifndef CPU_MAX_COUNT
#    define CPU_MAX_COUNT 0
#endif

#ifndef EPOLL_MAX_FDS
#    define EPOLL_MAX_FDS 1024
#endif

#ifndef FUTEX_HASH_BITS
#    define FUTEX_HASH_BITS 8
#endif

#ifndef INOTIFY_MAX_QUEUED_EVENTS
#    define INOTIFY_MAX_QUEUED_EVENTS 16384
#endif

#ifndef INOTIFY_MAX_USER_INSTANCES
#    define INOTIFY_MAX_USER_INSTANCES 128
#endif

#ifndef INOTIFY_MAX_USER_WATCHES
#    define INOTIFY_MAX_USER_WATCHES 8192
#endif

#ifndef INPUT_EVDEV_BUFSIZE
#    define INPUT_EVDEV_BUFSIZE 256
#endif

#ifndef KERNEL_HEAP_MAX_MIB
#    define KERNEL_HEAP_MAX_MIB 512
#endif

#ifndef KERNEL_LOG
#    define KERNEL_LOG 1
#endif

#ifndef PIPE_BUF_SIZE
#    define PIPE_BUF_SIZE 65536
#endif

#ifndef PROCESS_KERNEL_STACK
#    define PROCESS_KERNEL_STACK 65536
#endif

#ifndef PROCESS_MAX_FD
#    define PROCESS_MAX_FD 1024
#endif

#ifndef PROCESS_STACK_SIZE
#    define PROCESS_STACK_SIZE 8388608
#endif

#ifndef PROCESS_TABLE_SIZE
#    define PROCESS_TABLE_SIZE 4096
#endif

#ifndef SCHED_BASE_SLICE
#    define SCHED_BASE_SLICE 2
#endif

#ifndef SCHED_DEBUG_DEMO
#    define SCHED_DEBUG_DEMO 0
#endif

#ifndef SCHED_LATENCY
#    define SCHED_LATENCY 8
#endif

#ifndef SCHED_LOAD_BALANCE_INTERVAL
#    define SCHED_LOAD_BALANCE_INTERVAL 8
#endif

#ifndef SCHED_MIN_GRANULARITY
#    define SCHED_MIN_GRANULARITY 1
#endif

#ifndef SCHED_WAKEUP_GRANULARITY
#    define SCHED_WAKEUP_GRANULARITY 1
#endif

#ifndef SERIAL_BAUD_RATE
#    define SERIAL_BAUD_RATE 9600
#endif

#ifndef SERIAL_DATA_BITS
#    define SERIAL_DATA_BITS 8
#endif

#ifndef SERIAL_STOP_BITS
#    define SERIAL_STOP_BITS 1
#endif

#ifndef SOCK_ACCEPT_QUEUE_MAX
#    define SOCK_ACCEPT_QUEUE_MAX 4096
#endif

#ifndef SOCK_BUF_SIZE
#    define SOCK_BUF_SIZE 65536
#endif

#ifndef TIMER_HZ
#    define TIMER_HZ 1000
#endif

#ifndef TTY_BUF_SIZE
#    define TTY_BUF_SIZE 4096
#endif

#ifndef TTY_CORE_BUFFER_SIZE
#    define TTY_CORE_BUFFER_SIZE 4096
#endif

#ifndef TTY_DEFAULT_DEV
#    define TTY_DEFAULT_DEV "tty0"
#endif

#endif // INCLUDE_CONFIG_H_
