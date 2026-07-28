CC ?= gcc
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-function -g -O0 \
	-DCONFIG_SYSFS=1 -DCONFIG_UEVENT_HELPER=1 -I../include

KOBJECT_SOURCES := kobject_sysfs_test.c ../kernel/kobject.c ../fs/sysfs.c \
	../libs/glist/circular_list.c
EVDEV_QUEUE_SOURCES := evdev_queue_test.c ../drivers/input/evdev_queue.c
PS2_KEYBOARD_SOURCES := ps2_keyboard_test.c ../drivers/input/ps2_keyboard_protocol.c
USB_HID_SOURCES := usb_hid_test.c ../drivers/usb/hid_parser.c
XHCI_RING_SOURCES := xhci_ring_test.c ../drivers/usb/xhci_ring.c
USB_STORAGE_SOURCES := usb_storage_test.c ../drivers/usb/storage_protocol.c
PARTITION_SOURCES := partition_test.c ../drivers/block/partition.c
BLOCKDEV_PARTITION_SOURCES := blockdev_partition_test.c ../drivers/block/blockdev.c
NTFS_WRITE_SOURCES := ntfs_write_test.c
NTFS_WRITE_DEPS := ../fs/ntfs/ntfs_vfs.c
FUTEX_SOURCES := futex_test.c
NET_CORE_SOURCES ?= net_core_test.c net_core_mocks.c $(wildcard ../net/*.c)
NET_PATH_SOURCES ?= net_path_test.c $(wildcard ../net/*.c)
NET_IPV6_SOURCES ?= net_ipv6_test.c $(wildcard ../net/*.c)
NET_EPOLL_SOURCES ?= net_epoll_test.c net_epoll_mocks.c ../ipc/epoll.c
NET_TEST_CFLAGS := -Werror -Wno-error=address -Wno-error=type-limits -include net_test_compat.h -fno-builtin-strdup -ffunction-sections -fdata-sections
TARGETS := kobject_sysfs_test evdev_queue_test ps2_keyboard_test usb_hid_test xhci_ring_test usb_storage_test partition_test blockdev_partition_test ntfs_write_test path_at_test mmap_logic_test clock_sleep_test futex_test

.PHONY: test futex-test net-test clean

test: $(TARGETS)
	./kobject_sysfs_test
	./evdev_queue_test
	./ps2_keyboard_test
	./usb_hid_test
	./xhci_ring_test
	./usb_storage_test
	./partition_test
	./blockdev_partition_test
	./ntfs_write_test
	./path_at_test
	./mmap_logic_test
	./clock_sleep_test
	./futex_test

kobject_sysfs_test: $(KOBJECT_SOURCES)
	$(CC) $(CFLAGS) -fno-builtin-strdup -o $@ $(KOBJECT_SOURCES)

evdev_queue_test: $(EVDEV_QUEUE_SOURCES)
	$(CC) $(CFLAGS) -Werror -o $@ $(EVDEV_QUEUE_SOURCES)

ps2_keyboard_test: $(PS2_KEYBOARD_SOURCES)
	$(CC) $(CFLAGS) -Werror -o $@ $(PS2_KEYBOARD_SOURCES)

usb_hid_test: $(USB_HID_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -o $@ $(USB_HID_SOURCES)

xhci_ring_test: $(XHCI_RING_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -o $@ $(XHCI_RING_SOURCES)

usb_storage_test: $(USB_STORAGE_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -o $@ $(USB_STORAGE_SOURCES)

partition_test: $(PARTITION_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -o $@ $(PARTITION_SOURCES)

blockdev_partition_test: $(BLOCKDEV_PARTITION_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(BLOCKDEV_PARTITION_SOURCES)

ntfs_write_test: $(NTFS_WRITE_SOURCES) $(NTFS_WRITE_DEPS)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(NTFS_WRITE_SOURCES)

path_at_test: path_at_test.c ../fs/vfs.c
	$(CC) $(CFLAGS) -Werror -DVFS_PATH_TEST_ONLY -fno-builtin-strdup -o $@ $^

mmap_logic_test: mmap_logic_test.c ../kernel/syscall/mmap.c
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wl,--gc-sections -o $@ $^

clock_sleep_test: clock_sleep_test.c
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -o $@ $^

futex-test: futex_test
	./futex_test

futex_test: $(FUTEX_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(FUTEX_SOURCES)

net-test: net_core_test net_path_test net_ipv6_test net_epoll_test
	./net_core_test
	./net_path_test
	./net_ipv6_test
	./net_epoll_test

net_core_test: $(NET_CORE_SOURCES)
	$(CC) $(CFLAGS) $(NET_TEST_CFLAGS) -Wl,--gc-sections -o $@ $(NET_CORE_SOURCES)

net_path_test: $(NET_PATH_SOURCES)
	$(CC) $(CFLAGS) $(NET_TEST_CFLAGS) -Wl,--gc-sections -o $@ $(NET_PATH_SOURCES)

net_ipv6_test: $(NET_IPV6_SOURCES)
	$(CC) $(CFLAGS) $(NET_TEST_CFLAGS) -Wl,--gc-sections -o $@ $(NET_IPV6_SOURCES)

net_epoll_test: $(NET_EPOLL_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(NET_EPOLL_SOURCES)

clean:
	$(RM) $(TARGETS) net_core_test net_path_test net_ipv6_test net_epoll_test
