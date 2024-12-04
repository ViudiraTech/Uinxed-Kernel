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

OTHER_OBJECTS	= ./lib/klogo.lib ./lib/libos_terminal.lib ./lib/pl_readline.lib ./lib/libelf_parse.lib

CC				= gcc
LD				= ld
ASM				= nasm
RM				= rm
QEMU			= qemu-system-x86_64

C_FLAGS			= -Wall -Werror -Wcast-align -Winline -Wwrite-strings \
                  -c -I include -m32 -O3 -g -DNDEBUG -nostdinc -fno-pic \
                  -fno-builtin -fno-stack-protector

LD_FLAGS		= -T scripts/kernel.ld -m elf_i386 --strip-all
ASM_FLAGS		= -f elf -g -F stabs

all: info UxImage limine_iso
grub: info UxImage grub_iso

info:
	@printf "Uinxed-Kernel Compile Script.\n"
	@printf "Copyright 2020 ViudiraTech. All Rights Reserved.\n"
	@printf "Based on the GPL-3.0 open source license.\n"
	@echo

.c.o:
	@printf "\033[1;32m[Build]\033[0m Compiling Code Files $< ...\n"
	@$(CC) $(C_FLAGS) $< -o $@

.s.o:
	@printf "\033[1;32m[Build]\033[0m Compiling Assembly $< ...\n"
	@$(ASM) $(ASM_FLAGS) $<

UxImage: $(S_OBJECTS) $(C_OBJECTS)
	@echo
	@printf "\033[1;32m[Link]\033[0m Linking kernel...\n"
	@$(LD) $(LD_FLAGS) $(S_OBJECTS) $(C_OBJECTS) $(OTHER_OBJECTS) -o UxImage

.PHONY:limine
limine_iso: UxImage
	@echo
	@printf "\033[1;32m[ISO]\033[0m Packing ISO file...\n"
	@mkdir iso
	@cp -r boot/limine iso
	@cp $< iso

	@xorriso -as mkisofs -b limine/limine-bios-cd.bin -no-emul-boot -boot-info-table iso -o Uinxed.iso
	@rm -rf iso
	@printf "\033[1;32m[Done]\033[0m Compilation complete.\n"

.PHONY:grub
grub_iso: UxImage
	@echo
	@printf "\033[1;32m[ISO]\033[0m Packing ISO file...\n"
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
	@printf "\033[1;32m[Done]\033[0m Compilation complete.\n"

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
