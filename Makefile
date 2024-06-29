# Makefile -- Uinxed 编译文件（基于 GPL-3.0 开源协议）
# Copyright © 2020 ViudiraTech
# 源于 小严awa 撰写于 2024-6-23.

C_SOURCES = $(shell find . -name "*.c")
C_OBJECTS = $(patsubst %.c, %.o, $(C_SOURCES))
S_SOURCES = $(shell find . -name "*.s")
S_OBJECTS = $(patsubst %.s, %.o, $(S_SOURCES))

CC = gcc
LD = ld
ASM = nasm
RM = rm
QEMU = qemu-system-x86_64

C_FLAGS = -c -Wall -m32 -ggdb -gstabs+ -nostdinc -fno-pic -fno-builtin -fno-stack-protector -I include
LD_FLAGS = -T scripts/kernel.ld -m elf_i386 -nostdlib
ASM_FLAGS = -f elf -g -F stabs

all: link system.iso

.c.o:
	@echo Compiling Code Files $< ...
	$(CC) $(C_FLAGS) $< -o $@

.s.o:
	@echo Compiling Assembly $< ...
	$(ASM) $(ASM_FLAGS) $<

link:$(S_OBJECTS) $(C_OBJECTS)
	@echo Linking kernel...
	$(LD) $(LD_FLAGS) $(S_OBJECTS) $(C_OBJECTS) -o UxImage

.PHONY:iso
system.iso:UxImage
	@echo Packing ISO file...
	mkdir -p iso/boot/grub
	cp $< iso/boot/

	echo 'set timeout=3' > iso/boot/grub/grub.cfg
	echo 'set default=0' >> iso/boot/grub/grub.cfg

	echo 'menuentry "Uinxed"{' >> iso/boot/grub/grub.cfg
	echo '	multiboot /boot/UxImage' >> iso/boot/grub/grub.cfg
	echo '	boot' >> iso/boot/grub/grub.cfg
	echo '}' >> iso/boot/grub/grub.cfg

	grub-mkrescue --output=$@ iso
	rm -rf iso

.PHONY:clean
clean:
	$(RM) -f $(S_OBJECTS) $(C_OBJECTS) UxImage system.iso

.PHONY:qemu_iso
run:
	$(QEMU) -cdrom system.iso

.PHONY:qemu_kernel
runk:
	$(QEMU) -kernel UxImage
