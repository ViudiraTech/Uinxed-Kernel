# =====================================================
#
#		Makefile
#		Uinxed 编译脚本
#
#		2024/6/23 By Rainy101112
#		基于 GPL-3.0 开源协议
#		Copyright © 2020 ViudiraTech，保留最终解释权。
#
# =====================================================

C_SOURCES		= $(shell find . -name "*.c")
C_OBJECTS		= $(patsubst %.c, %.o, $(C_SOURCES))
S_SOURCES		= $(shell find . -name "*.s")
S_OBJECTS		= $(patsubst %.s, %.o, $(S_SOURCES))

OTHER_OBJECTS	= ./lib/klogo.lib ./lib/libos_terminal.lib ./lib/pl_readline.lib

CC				= gcc
LD				= ld
ASM				= nasm
RM				= rm
QEMU			= qemu-system-x86_64
C_FLAGS			= -Wall -c -I include -m32 -O3 -g -DNDEBUG -nostdinc -fno-pic -fno-builtin -fno-stack-protector
LD_FLAGS		= -T scripts/kernel.ld -m elf_i386 --strip-all
ASM_FLAGS		= -f elf -g -F stabs

all: info link limine_iso
grub: info link grub_iso

info:
	@echo Uinxed-Kernel Compile Script.
	@echo Copyright 2020 ViudiraTech. All Rights Reserved.
	@echo Based on the GPL-3.0 open source license.
	@echo

.c.o:
	@echo "\033[32m[Build]\033[0m" Compiling Code Files $< ...
	@$(CC) $(C_FLAGS) $< -o $@

.s.o:
	@echo "\033[32m[Build]\033[0m" Compiling Assembly $< ...
	@$(ASM) $(ASM_FLAGS) $<

link:$(S_OBJECTS) $(C_OBJECTS)
	@echo
	@echo "\033[32m[Link]\033[0m" Linking kernel...
	@$(LD) $(LD_FLAGS) $(S_OBJECTS) $(C_OBJECTS) $(OTHER_OBJECTS) -o UxImage

.PHONY:limine
limine_iso:UxImage
	@echo
	@echo "\033[32m[ISO]\033[0m" Packing ISO file...
	@mkdir iso
	@cp -r boot/limine iso
	@cp $< iso

	@xorriso -as mkisofs -b limine/limine-bios-cd.bin -no-emul-boot -boot-info-table iso -o Uinxed.iso
	@rm -rf iso
	@echo "\033[32m[Done]\033[0m" Compilation complete.

.PHONY:grub
grub_iso:UxImage
	@echo
	@echo "\033[32m[ISO]\033[0m" Packing ISO file...
	@mkdir -p iso/boot/grub
	@cp $< iso/boot/

	@echo 'set timeout=3' > iso/boot/grub/grub.cfg
	@echo 'set default=0' >> iso/boot/grub/grub.cfg

	@echo 'menuentry "Uinxed"{' >> iso/boot/grub/grub.cfg
	@echo '	multiboot /boot/UxImage' >> iso/boot/grub/grub.cfg
	@echo '	boot' >> iso/boot/grub/grub.cfg
	@echo '}' >> iso/boot/grub/grub.cfg

	@grub-mkrescue --locales="" --output=Uinxed.iso iso
	@rm -rf iso
	@echo "\033[32m[Done]\033[0m" Compilation complete.

.PHONY:clean
clean:
	$(RM) -f $(S_OBJECTS) $(C_OBJECTS) UxImage Uinxed.iso

.PHONY:qemu_iso
run:
	$(QEMU) -cdrom Uinxed.iso -serial stdio

.PHONY:qemu_iso_debug
run_db:
	$(QEMU) -cdrom Uinxed.iso -serial stdio -d in_asm

.PHONY:qemu_kernel
runk:
	$(QEMU) -kernel UxImage -serial stdio

.PHONY:qemu_kernel_debug
runk_db:
	$(QEMU) -kernel UxImage -serial stdio -d in_asm
