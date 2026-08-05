# =====================================================
#
#      Makefile
#      Uinxed-Kernel compile script
#
#      2024/6/23 By Rainy101112
#      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
#
# =====================================================

ifneq ($(wildcard .config),)
  include .config
else ifneq ($(wildcard .config-default),)
  include .config-default
else
  $(error No configuration file (.config or .config-default) found)
endif

ifeq ($(VERBOSE), 1)
  Q=
else
  Q=@
endif

ifeq ($(CONFIG_KERNEL_LOG), y)
  C_CONFIG += -DKERNEL_LOG=1
else
  C_CONFIG += -DKERNEL_LOG=0
endif

ifneq ($(CONFIG_TTY_DEFAULT_DEV),)
  C_CONFIG += -DTTY_DEFAULT_DEV=\"$(CONFIG_TTY_DEFAULT_DEV)\"
endif

ifneq ($(CONFIG_TTY_BUF_SIZE),)
  C_CONFIG += -DTTY_BUF_SIZE=$(CONFIG_TTY_BUF_SIZE)
endif

ifeq ($(CONFIG_UNIX98_PTYS),)
  C_CONFIG += -DCONFIG_UNIX98_PTYS=1
else ifeq ($(CONFIG_UNIX98_PTYS), y)
  C_CONFIG += -DCONFIG_UNIX98_PTYS=1
else
  C_CONFIG += -DCONFIG_UNIX98_PTYS=0
endif

ifeq ($(CONFIG_UNIX98_PTY_MAX),)
  C_CONFIG += -DCONFIG_UNIX98_PTY_MAX=64
else
  C_CONFIG += -DCONFIG_UNIX98_PTY_MAX=$(CONFIG_UNIX98_PTY_MAX)
endif

ifneq ($(CONFIG_SERIAL_BAUD_RATE),)
  C_CONFIG += -DSERIAL_BAUD_RATE=$(CONFIG_SERIAL_BAUD_RATE)
endif

ifneq ($(CONFIG_SERIAL_DATA_BITS),)
  C_CONFIG += -DSERIAL_DATA_BITS=$(CONFIG_SERIAL_DATA_BITS)
endif

ifneq ($(CONFIG_SERIAL_STOP_BITS),)
  C_CONFIG += -DSERIAL_STOP_BITS=$(CONFIG_SERIAL_STOP_BITS)
endif

ifeq ($(CONFIG_SERIAL),)
  C_CONFIG += -DCONFIG_SERIAL=1
else ifeq ($(CONFIG_SERIAL), y)
  C_CONFIG += -DCONFIG_SERIAL=1
else
  C_CONFIG += -DCONFIG_SERIAL=0
endif

ifneq ($(CONFIG_CPU_MAX_COUNT),)
  C_CONFIG += -DCPU_MAX_COUNT=$(CONFIG_CPU_MAX_COUNT)
endif

# Kernel code must not emit FPU/SSE/AVX instructions implicitly: any
# kernel floating-point use must opt in per-file (or per-function) with
# `#pragma GCC target("sse2")` and be wrapped in kernel_fpu_begin()/
# kernel_fpu_end() (see include/arch/fpu.h).
ifeq ($(CONFIG_CPU_FEATURE_FPU), y)
  C_CONFIG += -DCPU_FEATURE_FPU=1
else
  C_CONFIG += -DCPU_FEATURE_FPU=0
endif

ifeq ($(CONFIG_CPU_FEATURE_SSE), y)
  C_CONFIG += -DCPU_FEATURE_SSE=1
else
  C_CONFIG += -DCPU_FEATURE_SSE=0
endif

ifeq ($(CONFIG_CPU_FEATURE_AVX), y)
  C_CONFIG += -DCPU_FEATURE_AVX=1
else
  C_CONFIG += -DCPU_FEATURE_AVX=0
endif

ifeq ($(CONFIG_CPU_FEATURE_AVX512), y)
  C_CONFIG += -DCPU_FEATURE_AVX512=1
else
  C_CONFIG += -DCPU_FEATURE_AVX512=0
endif

ifneq ($(CONFIG_KERNEL_HEAP_MAX_SIZE),)
  C_CONFIG += -DKERNEL_HEAP_MAX_MIB=$(CONFIG_KERNEL_HEAP_MAX_SIZE)
endif

ifneq ($(CONFIG_SCHED_BASE_SLICE),)
  C_CONFIG += -DSCHED_BASE_SLICE=$(CONFIG_SCHED_BASE_SLICE)
endif

ifneq ($(CONFIG_SCHED_LATENCY),)
  C_CONFIG += -DSCHED_LATENCY=$(CONFIG_SCHED_LATENCY)
endif

ifneq ($(CONFIG_SCHED_MIN_GRANULARITY),)
  C_CONFIG += -DSCHED_MIN_GRANULARITY=$(CONFIG_SCHED_MIN_GRANULARITY)
endif

ifneq ($(CONFIG_SCHED_WAKEUP_GRANULARITY),)
  C_CONFIG += -DSCHED_WAKEUP_GRANULARITY=$(CONFIG_SCHED_WAKEUP_GRANULARITY)
endif

ifneq ($(CONFIG_SCHED_LOAD_BALANCE_INTERVAL),)
  C_CONFIG += -DSCHED_LOAD_BALANCE_INTERVAL=$(CONFIG_SCHED_LOAD_BALANCE_INTERVAL)
endif

ifeq ($(CONFIG_SCHED_DEBUG_DEMO), y)
  C_CONFIG += -DSCHED_DEBUG_DEMO=1
else
  C_CONFIG += -DSCHED_DEBUG_DEMO=0
endif

ifneq ($(CONFIG_PROCESS_MAX_FD),)
  C_CONFIG += -DPROCESS_MAX_FD=$(CONFIG_PROCESS_MAX_FD)
endif

ifneq ($(CONFIG_PROCESS_TABLE_SIZE),)
  C_CONFIG += -DPROCESS_TABLE_SIZE=$(CONFIG_PROCESS_TABLE_SIZE)
endif

ifneq ($(CONFIG_PROCESS_KERNEL_STACK_SIZE),)
  C_CONFIG += -DPROCESS_KERNEL_STACK=$(CONFIG_PROCESS_KERNEL_STACK_SIZE)
endif

ifneq ($(CONFIG_PROCESS_USER_STACK_SIZE),)
  C_CONFIG += -DPROCESS_STACK_SIZE=$(CONFIG_PROCESS_USER_STACK_SIZE)
endif

ifneq ($(CONFIG_PIPE_BUF_SIZE),)
  C_CONFIG += -DPIPE_BUF_SIZE=$(CONFIG_PIPE_BUF_SIZE)
endif

ifneq ($(CONFIG_SOCKET_BUF_SIZE),)
  C_CONFIG += -DSOCK_BUF_SIZE=$(CONFIG_SOCKET_BUF_SIZE)
endif

ifneq ($(CONFIG_SOCKET_ACCEPT_QUEUE_MAX),)
  C_CONFIG += -DSOCK_ACCEPT_QUEUE_MAX=$(CONFIG_SOCKET_ACCEPT_QUEUE_MAX)
endif

ifneq ($(CONFIG_EPOLL_MAX_FDS),)
  C_CONFIG += -DEPOLL_MAX_FDS=$(CONFIG_EPOLL_MAX_FDS)
endif

ifneq ($(CONFIG_FUTEX_HASH_BITS),)
  C_CONFIG += -DFUTEX_HASH_BITS=$(CONFIG_FUTEX_HASH_BITS)
endif

ifneq ($(CONFIG_INOTIFY_MAX_QUEUED_EVENTS),)
  C_CONFIG += -DINOTIFY_MAX_QUEUED_EVENTS=$(CONFIG_INOTIFY_MAX_QUEUED_EVENTS)
endif

ifneq ($(CONFIG_INOTIFY_MAX_USER_INSTANCES),)
  C_CONFIG += -DINOTIFY_MAX_USER_INSTANCES=$(CONFIG_INOTIFY_MAX_USER_INSTANCES)
endif

ifneq ($(CONFIG_INOTIFY_MAX_USER_WATCHES),)
  C_CONFIG += -DINOTIFY_MAX_USER_WATCHES=$(CONFIG_INOTIFY_MAX_USER_WATCHES)
endif

ifeq ($(CONFIG_NETLINK), y)
  C_CONFIG += -DCONFIG_NETLINK=1
else
  C_CONFIG += -DCONFIG_NETLINK=0
endif

ifeq ($(CONFIG_SYSVIPC), y)
  C_CONFIG += -DCONFIG_SYSVIPC=1
else
  C_CONFIG += -DCONFIG_SYSVIPC=0
endif

ifeq ($(CONFIG_POSIX_MQ), y)
  C_CONFIG += -DCONFIG_POSIX_MQ=1
else
  C_CONFIG += -DCONFIG_POSIX_MQ=0
endif

ifeq ($(CONFIG_NET), y)
  C_CONFIG += -DCONFIG_NET=1
else
  C_CONFIG += -DCONFIG_NET=0
endif

ifeq ($(CONFIG_INET), y)
  C_CONFIG += -DCONFIG_INET=1
else
  C_CONFIG += -DCONFIG_INET=0
endif

ifeq ($(CONFIG_E1000), y)
  C_CONFIG += -DCONFIG_E1000=1
else
  C_CONFIG += -DCONFIG_E1000=0
endif

ifeq ($(CONFIG_PS2_KEYBOARD_MOUSE), y)
  C_CONFIG += -DCONFIG_PS2_KEYBOARD_MOUSE=1
else
  C_CONFIG += -DCONFIG_PS2_KEYBOARD_MOUSE=0
endif

ifeq ($(CONFIG_PARPORT), y)
  C_CONFIG += -DCONFIG_PARPORT=1
else
  C_CONFIG += -DCONFIG_PARPORT=0
endif

ifeq ($(CONFIG_INPUT_EVDEV), y)
  C_CONFIG += -DCONFIG_INPUT_EVDEV=1
else
  C_CONFIG += -DCONFIG_INPUT_EVDEV=0
endif

ifneq ($(CONFIG_INPUT_EVDEV_BUFSIZE),)
  C_CONFIG += -DINPUT_EVDEV_BUFSIZE=$(CONFIG_INPUT_EVDEV_BUFSIZE)
endif

ifeq ($(CONFIG_USB), y)
  C_CONFIG += -DCONFIG_USB=1
else
  C_CONFIG += -DCONFIG_USB=0
endif

ifeq ($(CONFIG_USB_UHCI), y)
  C_CONFIG += -DCONFIG_USB_UHCI=1
else
  C_CONFIG += -DCONFIG_USB_UHCI=0
endif

ifeq ($(CONFIG_USB_OHCI), y)
  C_CONFIG += -DCONFIG_USB_OHCI=1
else
  C_CONFIG += -DCONFIG_USB_OHCI=0
endif

ifeq ($(CONFIG_USB_EHCI), y)
  C_CONFIG += -DCONFIG_USB_EHCI=1
else
  C_CONFIG += -DCONFIG_USB_EHCI=0
endif

ifeq ($(CONFIG_USB_XHCI), y)
  C_CONFIG += -DCONFIG_USB_XHCI=1
else
  C_CONFIG += -DCONFIG_USB_XHCI=0
endif

ifeq ($(CONFIG_USB_HID), y)
  C_CONFIG += -DCONFIG_USB_HID=1
else
  C_CONFIG += -DCONFIG_USB_HID=0
endif

ifeq ($(CONFIG_USB_STORAGE), y)
  C_CONFIG += -DCONFIG_USB_STORAGE=1
else
  C_CONFIG += -DCONFIG_USB_STORAGE=0
endif

ifeq ($(CONFIG_DRM), y)
  C_CONFIG += -DCONFIG_DRM=1
else
  C_CONFIG += -DCONFIG_DRM=0
endif

ifeq ($(CONFIG_BOOT_LOGO), y)
  C_CONFIG += -DBOOT_LOGO=1
else
  C_CONFIG += -DBOOT_LOGO=0
endif

ifeq ($(CONFIG_VIRTIO), y)
  C_CONFIG += -DCONFIG_VIRTIO=1
else
  C_CONFIG += -DCONFIG_VIRTIO=0
endif

ifeq ($(CONFIG_VIRTIO_GPU), y)
  C_CONFIG += -DCONFIG_VIRTIO_GPU=1
else
  C_CONFIG += -DCONFIG_VIRTIO_GPU=0
endif

ifeq ($(CONFIG_SOUND_HDA), y)
  C_CONFIG += -DCONFIG_SOUND_HDA=1
else
  C_CONFIG += -DCONFIG_SOUND_HDA=0
endif

ifeq ($(CONFIG_SOUND_SB16), y)
  C_CONFIG += -DCONFIG_SOUND_SB16=1
else
  C_CONFIG += -DCONFIG_SOUND_SB16=0
endif

ifeq ($(CONFIG_PCSPKR), y)
  C_CONFIG += -DCONFIG_PCSPKR=1
else
  C_CONFIG += -DCONFIG_PCSPKR=0
endif

ifeq ($(CONFIG_I2C), y)
  C_CONFIG += -DCONFIG_I2C=1
else
  C_CONFIG += -DCONFIG_I2C=0
endif

ifeq ($(CONFIG_TPM), y)
  C_CONFIG += -DCONFIG_TPM=1
else
  C_CONFIG += -DCONFIG_TPM=0
endif

ifeq ($(CONFIG_SYSFS), y)
  C_CONFIG += -DCONFIG_SYSFS=1
else
  C_CONFIG += -DCONFIG_SYSFS=0
endif

ifeq ($(CONFIG_UEVENT_HELPER), y)
  C_CONFIG += -DCONFIG_UEVENT_HELPER=1
else
  C_CONFIG += -DCONFIG_UEVENT_HELPER=0
endif

ifeq ($(CONFIG_NTFS_FS), y)
  C_CONFIG += -DCONFIG_NTFS_FS=1
else
  C_CONFIG += -DCONFIG_NTFS_FS=0
endif

ifeq ($(CONFIG_FAT_FS), y)
  C_CONFIG += -DCONFIG_FAT_FS=1
else
  C_CONFIG += -DCONFIG_FAT_FS=0
endif

ifeq ($(CONFIG_ISO9660_FS), y)
  C_CONFIG += -DCONFIG_ISO9660_FS=1
else
  C_CONFIG += -DCONFIG_ISO9660_FS=0
endif

ifeq ($(CONFIG_EXTFS), y)
  C_CONFIG += -DCONFIG_EXTFS=1
else
  C_CONFIG += -DCONFIG_EXTFS=0
endif

ifeq ($(CONFIG_CGROUP), y)
  C_CONFIG += -DCONFIG_CGROUP=1
else
  C_CONFIG += -DCONFIG_CGROUP=0
endif

ifeq ($(CONFIG_MODULES), y)
  C_CONFIG += -DCONFIG_MODULES=1
else
  C_CONFIG += -DCONFIG_MODULES=0
endif

ifeq ($(CONFIG_MODULE_FORCE_LOAD), y)
  C_CONFIG += -DCONFIG_MODULE_FORCE_LOAD=1
else
  C_CONFIG += -DCONFIG_MODULE_FORCE_LOAD=0
endif

ifeq ($(CONFIG_MODULE_FORCE_UNLOAD), y)
  C_CONFIG += -DCONFIG_MODULE_FORCE_UNLOAD=1
else
  C_CONFIG += -DCONFIG_MODULE_FORCE_UNLOAD=0
endif

ifeq ($(CONFIG_MODULE_SIG_FORCE), y)
  C_CONFIG += -DCONFIG_MODULE_SIG_FORCE=1
else
  C_CONFIG += -DCONFIG_MODULE_SIG_FORCE=0
endif

ifneq ($(CONFIG_MODULE_MAX_SIZE),)
  C_CONFIG += -DCONFIG_MODULE_MAX_SIZE_MIB=$(CONFIG_MODULE_MAX_SIZE)
endif

C_SOURCES      := $(shell find * -name "*.c" -not -path "tools/*" -not -path "assets/*")
C_HEADERS      := $(shell find * -name "*.h")
OBJS           := $(C_SOURCES:%.c=%.o)
DEPS           := $(OBJS:%.o=%.d)
ELFS           := $(shell find * -name "*.elf")
LIBS           := $(wildcard libs/lib*.a)
PWD            := $(shell pwd)

HOST_CC        := $(CC)
HOST_CFLAGS    := -Wall -Wextra -O2

QEMU           := qemu-system-x86_64
QEMU_FLAGS     := -machine q35 -bios assets/ovmf-code.fd -serial stdio

TOOL_C_SOURCES := $(wildcard tools/*.c)
TOOL_TARGETS   := $(TOOL_C_SOURCES:%.c=%.elf)

# If you want to get more details of `dump_stack`, you need to replace `-O3` with `-O0` or '-Os'.
# `-fno-optimize-sibling-calls` is for `dump_stack` to work properly.
CC_FLAGS       := -Wall -Wextra -Wno-unused-function -O3 -g3 -m64 -fpie -ffreestanding -fno-optimize-sibling-calls -fno-stack-protector -fno-omit-frame-pointer -mstackrealign -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 -I include -MMD
LD_FLAGS       := -nostdlib -pie -T assets/linker.ld -m elf_x86_64

all: Uinxed-x64.iso

info:
	$(Q)printf "Uinxed Compiling Script - Apache License Version 2.0.\n\n"

%.o: %.c
	$(Q)printf "  CC      $@\n"
	$(Q)$(CC) $(CC_FLAGS) $(C_CONFIG) -MT $@ -c -o $@ $<

%.fmt: %
	$(Q)printf "  FORMAT  $<\n"
	$(Q)clang-format -i $<

%.tidy: %
	$(Q)printf "  TIDY    $<\n"
	$(Q)clang-tidy $< -- $(CC_FLAGS)

tools/%.elf: tools/%.c
	$(Q)printf "  HOSTCC  $@\n"
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

assets/Limine/init.elf: assets/init/main.c
	$(Q)printf "  HOSTCC  $@\n"
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -static -o $@ $<

UxImage: $(TOOL_TARGETS) $(OBJS) $(LIBS)
	$(Q)printf "  LD      $@\n"
	$(Q)$(LD) $(LD_FLAGS) -o $@ $(filter-out $(TOOL_TARGETS),$^)

Uinxed-x64.iso: info UxImage assets/Limine/init.elf
	$(Q)printf "  XORRISO $@\n\n"
	$(Q)cp -a assets/Limine iso
	$(Q)cp $(word 2,$^) iso/EFI/Boot
	$(Q)xorriso -as mkisofs -R -r -J -b Limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
                -hfsplus -apm-block-size 2048 -efi-boot-part --efi-boot-image --protective-msdos-label \
                --efi-boot Limine/limine-uefi-cd.bin -o $@ iso
	$(Q)$(RM) -rf iso
	$(Q)printf "Kernel: $(word 2,$^) is ready.\n"
	$(Q)printf "Image: $@ is ready.\n"
	$(Q)printf "Compilation complete.\n"

.PHONY: help run clean format check gen.clangd menuconfig

help: info
	$(Q)printf "Uinxed-Kernel Makefile Usage:\n"
	$(Q)printf "  make all         - Build the entire project.\n"
	$(Q)printf "  make run         - Run the Uinxed-x64.iso in QEMU.\n"
	$(Q)printf "  make clean       - Clean all generated files.\n"
	$(Q)printf "  make format      - Format all source files using clang-format.\n"
	$(Q)printf "  make check       - Run static code checks using clang-tidy.\n"
	$(Q)printf "  make gen.clangd  - Generate .clangd configuration file.\n"
	$(Q)printf "  make menuconfig  - Run menuconfig to configure the kernel.\n"
	$(Q)printf "  make help        - Display this help message.\n"

run: info Uinxed-x64.iso
	$(QEMU) $(QEMU_FLAGS) -cdrom $(word 2,$^)

clean: info
	$(Q)$(RM) $(OBJS) $(DEPS) $(ELFS) UxImage Uinxed-x64.iso
	$(Q)printf "Clean completed.\n"

format: info $(C_SOURCES:%=%.fmt) $(C_HEADERS:%=%.fmt)
	$(Q)find . -type f ! -path './.git/*' -exec dos2unix -q {} +
	$(Q)printf "\nCode Format complete.\n"

check: info $(C_SOURCES:%=%.tidy) $(C_HEADERS:%=%.tidy)
	$(Q)printf "\nCode Checks complete.\n"

gen.clangd: info
	$(Q)$(RM) -f .clangd
	$(Q)echo "# Generated by Makefile" >> .clangd
	$(Q)sed "s/\$${workspaceFolder}/$(subst /,\/,${PWD})/g" .clangd_template >> .clangd
	$(Q)printf ".clangd configuration generated.\n"

menuconfig: info
	$(Q)kconfig-mconf Kconfig

-include $(DEPS)
