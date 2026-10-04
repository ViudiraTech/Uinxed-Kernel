# =====================================================
#
#      Makefile
#      Uinxed-Kernel compile script
#
#      2024/6/23 By Rainy101112
#      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
#
# =====================================================

include scripts/kconfig.mk

ifeq ($(VERBOSE), 1)
  Q=
else
  Q=@
endif

# Source discovery
C_SOURCES      := $(shell find * -name "*.c" -not -path "assets/*" -not -path "docs/*" -not -path "scripts/*" -not -path "tools/*" -not -path "build/*" -not -path "vdso/*")
C_HEADERS      := $(shell find * -name "*.h" -not -path "assets/*" -not -path "docs/*" -not -path "scripts/*" -not -path "tools/*" -not -path "build/*" -not -path "vdso/*")
JOBS           ?= $(shell nproc 2>/dev/null || echo 1)
OBJS           := $(C_SOURCES:%.c=%.o)
# The vDSO image is generated at build time, so it is not in C_SOURCES.
VDSO_IMAGE     := vdso/vdso_image.c
OBJS           += $(VDSO_IMAGE:%.c=%.o)
DEPS           := $(OBJS:%.o=%.d)
ELFS           := $(shell find * -name "*.elf")
LIBS           := $(wildcard libs/lib*.a)
PWD            := $(shell pwd)

# Host toolchain
HOST_CC        := $(CC)
HOST_CFLAGS    := -Wall -Wextra -O2
TOOL_C_SOURCES := $(wildcard tools/*.c)
TOOL_TARGETS   := $(TOOL_C_SOURCES:%.c=%.elf)

# Compiler and linker flags
CC_OPT         := -O3 -g3
CC_TARGET      := -ffreestanding -fpie -mno-red-zone -mno-sse -mno-mmx -mno-80387
CC_CODEGEN     := -fno-stack-protector -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-math-errno
CC_WARN        := -Wall -Wextra -Werror -Wno-unused-function
CC_SECTIONS    := -flto=auto -ffunction-sections -fdata-sections
CC_INCLUDES    := -Iinclude -include include/kernel/config.h -MMD
CC_INLINE       = -fno-inline-functions $(if $(findstring clang,$(CC)),-finline-hint-functions,-fno-inline-small-functions -fno-inline-functions-called-once)

CC_FLAGS        = $(CC_OPT) $(CC_TARGET) $(CC_CODEGEN) $(CC_WARN) $(CC_SECTIONS) $(CC_INLINE) $(CC_INCLUDES)
LD_FLAGS       := $(CC_OPT) $(CC_SECTIONS) $(CC_INLINE) -nostdlib -pie -T assets/linker.ld -Wl,--gc-sections -Wl,--build-id=none
CLANGD_DROP    := $(CC_SECTIONS) $(CC_INLINE) -MMD

# Runtime / image tooling
QEMU           := qemu-system-x86_64
QEMU_FLAGS     := -machine q35 -bios assets/ovmf-code.fd -serial stdio -m 1G
ISO_DIR        := iso
ISO_BOOT_DIR   := $(ISO_DIR)/EFI/Boot

all: Uinxed-x64.iso

info:
	$(Q)printf "Uinxed Compiling Script - Apache License Version 2.0.\n\n"

VDSO_CFLAGS    := -O2 -fPIC -fno-stack-protector -fno-common -fno-builtin -fno-asynchronous-unwind-tables
VDSO_LD_FLAGS  := -nostdlib -shared -Bsymbolic -Wl,--hash-style=sysv -Wl,--build-id=none
VDSO_CC        ?= cc

# The vDSO is a userspace artefact linked with its own script: one loadable
# segment starting at virtual address 0, with the ELF header first and the clock
# data page page-aligned behind the code.  The kernel copies the whole image once
# and maps its own shared data page over the image's own offset for it.
vdso/vdso.so.dbg: vdso/vdso.c vdso/vdso.lds
	$(Q)printf "  VDSO    $@\n"
	$(Q)$(VDSO_CC) $(VDSO_CFLAGS) -Iinclude -Wl,-T,vdso/vdso.lds -Wl,-soname=linux-vdso.so.1 $(VDSO_LD_FLAGS) -o $@ vdso/vdso.c

vdso/vdso.so: vdso/vdso.so.dbg
	$(Q)printf "  STRIP   $@\n"
	$(Q)objcopy -S $< $@

$(VDSO_IMAGE): vdso/vdso.so scripts/vdso2c.py
	$(Q)printf "  GEN     $@\n"
	$(Q)python3 scripts/vdso2c.py vdso/vdso.so vdso_image > $@

$(VDSO_IMAGE:%.c=%.o): $(VDSO_IMAGE)

%.o: %.c
	$(Q)printf "  CC      $@\n"
	$(Q)$(CC) $(CC_FLAGS) $(C_CONFIG) -MT $@ -c -o $@ $<

%.fmt: %
	$(Q)printf "  FORMAT  $<\n"
	$(Q)clang-format -i $<

%.tidy: CC = clang
%.tidy: %
	$(Q)printf "  TIDY    $<\n"
	$(Q)tmp=$$(mktemp); clang-tidy $< -- $(CC_FLAGS) $(C_CONFIG) > $$tmp 2>&1; status=$$?; grep -vE '[0-9]+ warnings? generated\.|Suppressed [0-9]+ warnings? \(' $$tmp; if [ $$status -ne 0 ] || grep -qE ':[0-9]+:[0-9]+: (warning|error):' $$tmp; then rm -f $$tmp; exit 1; fi; rm -f $$tmp

tools/%.elf: tools/%.c
	$(Q)printf "  HOSTCC  $@\n"
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

UxImage: $(TOOL_TARGETS) $(OBJS) $(LIBS)
	$(Q)printf "  LD      $@\n"
	$(Q)$(CC) $(LD_FLAGS) -o $@ $(filter-out $(TOOL_TARGETS),$^)
	$(Q)printf "  NM      System.map\n"
	$(Q)nm -n $@ > System.map

Uinxed-x64.iso: info UxImage
	$(Q)printf "  XORRISO $@\n\n"

	$(Q)mkdir -p $(ISO_DIR) $(ISO_BOOT_DIR)
	$(Q)cp -a assets/Limine/* $(ISO_DIR)/
	$(Q)cp $(word 2,$^) $(ISO_BOOT_DIR)/

	$(Q)trap '$(RM) -rf $(ISO_DIR); exit 1' INT TERM HUP; \
		xorriso -as mkisofs -R -r -J -b Limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
			-hfsplus -apm-block-size 2048 -efi-boot-part --efi-boot-image --protective-msdos-label \
			--efi-boot Limine/limine-uefi-cd.bin -o $@ $(ISO_DIR); \
		status=$$?; \
		$(RM) -rf $(ISO_DIR); \
		if [ $$status -eq 0 ]; then \
			printf "Kernel: $(word 2,$^) is ready.\n"; \
			printf "Image: $@ is ready.\n"; \
			printf "Symbols: System.map is ready.\n"; \
			printf "Compilation complete.\n"; \
		fi; \
		exit $$status

.PHONY: all info help run clean format check gen.clangd menuconfig

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
	$(Q)out=0; for f in $(OBJS) $(DEPS) $(ELFS) UxImage Uinxed-x64.iso System.map vdso/vdso.so vdso/vdso.so.dbg $(VDSO_IMAGE); do if [ -e "$$f" ]; then printf "  RM      $$f\n"; out=1; fi; done; [ "$$out" = 1 ] && printf "\n"; true
	$(Q)$(RM) $(OBJS) $(DEPS) $(ELFS) UxImage Uinxed-x64.iso System.map \
		vdso/vdso.so vdso/vdso.so.dbg $(VDSO_IMAGE) $(VDSO_IMAGE:%.c=%.o) $(VDSO_IMAGE:%.c=%.d)
	$(Q)printf "Clean completed.\n"

format: info
	$(Q)$(MAKE) --no-print-directory -j$(JOBS) $(C_SOURCES:%=%.fmt) $(C_HEADERS:%=%.fmt)
	$(Q)find . -type f ! -path './.git/*' -print0 | xargs -0 grep -IlZ '' | xargs -0 -r dos2unix -q
	$(Q)for f in $(C_SOURCES) $(C_HEADERS); do if [ -s "$$f" ] && [ -n "$$(tail -c1 "$$f")" ]; then echo >> "$$f"; fi; done
	$(Q)printf "\nCode Format complete.\n"

check: info
	$(Q)$(MAKE) --no-print-directory -j$(JOBS) -k $(C_SOURCES:%=%.tidy) || exit 1
	$(Q)printf "\nCode Checks complete.\n"

gen.clangd: info
	$(Q)printf "  GEN     .clangd\n\n"
	$(Q)$(RM) -f .clangd
	$(Q)printf -- '---\nCompileFlags:\n  Add:\n' > .clangd
	$(Q)for f in $(filter-out $(CLANGD_DROP),$(CC_FLAGS)); do printf '    - "%s"\n' "$$f" >> .clangd; done
	$(Q)printf 'Diagnostics:\n  ClangTidy:\n    FastCheckFilter: Strict\n...\n' >> .clangd
	$(Q)printf ".clangd configuration generated.\n"

menuconfig: info
	$(Q)kconfig-mconf Kconfig

-include $(DEPS)
