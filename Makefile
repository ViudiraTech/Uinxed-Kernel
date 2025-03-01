# =====================================================
#
#		Makefile
#		Uinxed 编译脚本
#
#		2024/6/23 By Rainy101112
#		基于 GPL-3.0 开源协议
#		Copyright © 2020 ViudiraTech，开放所有权利。
#
# =====================================================

ifeq ($(VERBOSE), 1)
  V=
  Q=
else
  V=@printf "\033[1;32m[Build]\033[0m $@ ...\n";
  Q=@
endif

BUILDDIR		?= build

C_SOURCES		:= $(shell find * -name "*.c")
S_SOURCES		:= $(shell find * -name "*.s")
OBJS			:= $(C_SOURCES:%.c=$(BUILDDIR)/%.o) $(S_SOURCES:%.s=$(BUILDDIR)/%.o)
DEPS			:= $(OBJS:%.o=%.d)
LIBS			:= $(wildcard lib/lib*.a)

CC				= gcc
LD				= ld
ASM				= nasm
RM				= rm
QEMU			= qemu-system-x86_64
QEMU_FLAGS		= -serial stdio -audiodev pa,id=speaker -machine pcspk-audiodev=speaker

C_FLAGS			= -MMD -Wall -Werror -Wcast-align -Winline -Wwrite-strings \
                  -c -I include -m32 -O3 -g -DNDEBUG -nostdinc -fno-pic \
				  -mno-mmx -mno-sse -mno-sse2 \
                  -fno-builtin -fno-stack-protector

LD_FLAGS		= -T scripts/kernel.ld -m elf_i386 --strip-all
ASM_FLAGS		= -f elf -g -F stabs

all: info Uinxed.iso

.PHONY: info
info:
	@printf "Uinxed-Kernel Compile Script.\n"
	@printf "Copyright 2020 ViudiraTech. Open all rights.\n"
	@printf "Based on the GPL-3.0 open source license.\n"
	@echo

makedirs:
	$(Q)mkdir -p $(dir $(OBJS))

$(BUILDDIR)/%.o: %.c | makedirs
	$(V)$(CC) $(C_FLAGS) -o $@ $<

$(BUILDDIR)/%.o: %.s | makedirs
	$(V)$(ASM) $(ASM_FLAGS) -o $@ $<

UxImage: $(OBJS) $(LIBS)
	$(V)$(LD) $(LD_FLAGS) -o $@ $^

.PHONY: grub_iso
grub_iso: Uinxed.iso

Uinxed.iso: UxImage
	@echo
	@printf "\033[1;32m[ISO]\033[0m Packing ISO file...\n"
	@mkdir -p iso/boot/grub
	@cp $< iso/boot/

	@echo 'set timeout=3' > iso/boot/grub/grub.cfg
	@echo 'set default=0' >> iso/boot/grub/grub.cfg

	@echo 'menuentry "Uinxed"{' >> iso/boot/grub/grub.cfg
	@echo '	multiboot /boot/UxImage console=tty0' >> iso/boot/grub/grub.cfg
	@echo '	boot' >> iso/boot/grub/grub.cfg
	@echo '}' >> iso/boot/grub/grub.cfg

	@grub-mkrescue --locales="" --output=Uinxed.iso iso
	@rm -rf iso
	@printf "\033[1;32m[Done]\033[0m Compilation complete.\n"

.PHONY: config
menuconfig:
	@kconfig-mconf Kconfig
	@python3 Kconfig.py

.PHONY: clean
clean:
	$(Q)$(RM) -f $(OBJS) $(DEPS) UxImage Uinxed.iso .config .config.old

.PHONY: run
run: Uinxed.iso
	$(QEMU) $(QEMU_FLAGS) -cdrom Uinxed.iso

.PHONY: run_db
run_db: Uinxed.iso
	$(QEMU) $(QEMU_FLAGS) -cdrom Uinxed.iso -d in_asm

.PHONY: runk
runk: UxImage
	$(QEMU) $(QEMU_FLAGS) -kernel UxImage

.PHONY: runk_db
runk_db: UxImage
	$(QEMU) $(QEMU_FLAGS) -kernel UxImage -d in_asm

.PRECIOUS: $(OBJS)

-include $(DEPS)
