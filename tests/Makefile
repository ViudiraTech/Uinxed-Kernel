CC ?= gcc
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-function -g -O0 \
	-DCONFIG_SYSFS=1 -DCONFIG_UEVENT_HELPER=1 -I../include

KOBJECT_SOURCES := kobject_sysfs_test.c ../kernel/kobject.c ../fs/sysfs.c \
	../libs/glist/circular_list.c
EVDEV_QUEUE_SOURCES := evdev_queue_test.c ../drivers/input/evdev_queue.c
PS2_KEYBOARD_SOURCES := ps2_keyboard_test.c ../drivers/input/ps2_keyboard_protocol.c
PARTITION_SOURCES := partition_test.c ../drivers/block/partition.c
BLOCKDEV_PARTITION_SOURCES := blockdev_partition_test.c ../drivers/block/blockdev.c
NTFS_WRITE_SOURCES := ntfs_write_test.c
NTFS_WRITE_DEPS := ../fs/ntfs/ntfs_vfs.c
NET_CORE_SOURCES ?= net_core_test.c $(wildcard ../net/*.c)
NET_PATH_SOURCES ?= net_path_test.c $(wildcard ../net/*.c)
NET_TEST_CFLAGS := -Werror -Wno-error=address -Wno-error=type-limits -include net_test_compat.h -fno-builtin-strdup -ffunction-sections -fdata-sections
TARGETS := kobject_sysfs_test evdev_queue_test ps2_keyboard_test partition_test blockdev_partition_test ntfs_write_test

.PHONY: test net-test clean

test: $(TARGETS)
	./kobject_sysfs_test
	./evdev_queue_test
	./ps2_keyboard_test
	./partition_test
	./blockdev_partition_test
	./ntfs_write_test

kobject_sysfs_test: $(KOBJECT_SOURCES)
	$(CC) $(CFLAGS) -fno-builtin-strdup -o $@ $(KOBJECT_SOURCES)

evdev_queue_test: $(EVDEV_QUEUE_SOURCES)
	$(CC) $(CFLAGS) -Werror -o $@ $(EVDEV_QUEUE_SOURCES)

ps2_keyboard_test: $(PS2_KEYBOARD_SOURCES)
	$(CC) $(CFLAGS) -Werror -o $@ $(PS2_KEYBOARD_SOURCES)

partition_test: $(PARTITION_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -o $@ $(PARTITION_SOURCES)

blockdev_partition_test: $(BLOCKDEV_PARTITION_SOURCES)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(BLOCKDEV_PARTITION_SOURCES)

ntfs_write_test: $(NTFS_WRITE_SOURCES) $(NTFS_WRITE_DEPS)
	$(CC) $(CFLAGS) -Werror -fno-builtin-strdup -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(NTFS_WRITE_SOURCES)

net-test: net_core_test net_path_test
	./net_core_test
	./net_path_test

net_core_test: $(NET_CORE_SOURCES)
	$(CC) $(CFLAGS) $(NET_TEST_CFLAGS) -Wl,--gc-sections -o $@ $(NET_CORE_SOURCES)

net_path_test: $(NET_PATH_SOURCES)
	$(CC) $(CFLAGS) $(NET_TEST_CFLAGS) -Wl,--gc-sections -o $@ $(NET_PATH_SOURCES)

clean:
	$(RM) $(TARGETS) net_core_test net_path_test
