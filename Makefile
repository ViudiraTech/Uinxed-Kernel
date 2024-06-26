# patsubst 处理所有在 C_SOURCES 字列中的字（一列文件名），如果它的 结尾是 '.c'，就用 '.o' 把 '.c' 取代
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

# The automatic variable `$<' is just the first prerequisite
.c.o:
	@echo 编译代码文件 $< ...
	$(CC) $(C_FLAGS) $< -o $@

.s.o:
	@echo 编译汇编文件 $< ...
	$(ASM) $(ASM_FLAGS) $<

link:$(S_OBJECTS) $(C_OBJECTS)
	@echo 链接内核文件...
	$(LD) $(LD_FLAGS) $(S_OBJECTS) $(C_OBJECTS) -o UxImage

.PHONY:clean
clean:
	$(RM) -f $(S_OBJECTS) $(C_OBJECTS) UxImage system.iso

.PHONY:iso
system.iso:UxImage
	mkdir -p iso/boot/grub
	cp $< iso/boot/
	echo 'set timeout=3' > iso/boot/grub/grub.cfg
	echo 'set default=0' >> iso/boot/grub/grub.cfg
	echo 'menuentry "system"{' >> iso/boot/grub/grub.cfg
	echo '	multiboot /boot/UxImage' >> iso/boot/grub/grub.cfg
	echo '	boot' >> iso/boot/grub/grub.cfg
	echo '}' >> iso/boot/grub/grub.cfg
	grub-mkrescue --output=$@ iso
	rm -rf iso

.PHONY:qemu_iso
qemu_iso:
	$(QEMU) -cdrom system.iso

.PHONY:qemu_kernel
qemu_kernel:
	$(QEMU) -kernel UxImage
