CC ?= gcc
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-function -g -O0 \
	-DCONFIG_SYSFS=1 -DCONFIG_UEVENT_HELPER=1 -I../include

KOBJECT_SOURCES := kobject_sysfs_test.c ../kernel/kobject.c ../fs/sysfs.c \
	../libs/glist/circular_list.c
EVDEV_QUEUE_SOURCES := evdev_queue_test.c ../drivers/input/evdev_queue.c
PS2_KEYBOARD_SOURCES := ps2_keyboard_test.c ../drivers/input/ps2_keyboard_protocol.c
TARGETS := kobject_sysfs_test evdev_queue_test ps2_keyboard_test

.PHONY: test clean

test: $(TARGETS)
	./kobject_sysfs_test
	./evdev_queue_test
	./ps2_keyboard_test

kobject_sysfs_test: $(KOBJECT_SOURCES)
	$(CC) $(CFLAGS) -fno-builtin-strdup -o $@ $(KOBJECT_SOURCES)

evdev_queue_test: $(EVDEV_QUEUE_SOURCES)
	$(CC) $(CFLAGS) -Werror -o $@ $(EVDEV_QUEUE_SOURCES)

ps2_keyboard_test: $(PS2_KEYBOARD_SOURCES)
	$(CC) $(CFLAGS) -Werror -o $@ $(PS2_KEYBOARD_SOURCES)

clean:
	$(RM) $(TARGETS)
