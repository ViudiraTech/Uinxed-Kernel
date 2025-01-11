#include "multiboot.h"
#include "cmos.h"
#include "boot.h"
#include "memory.h"
#include "acpi.h"
#include "elf.h"
#include "string.h"

#define MULTIBOOT_HEADER_FLAGS (MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMORY_INFO | MULTIBOOT_VIDEO_MODE)

static const struct multiboot_header multiboot __attribute__((used, section(".multiboot"))) = {
	.magic = MULTIBOOT_HEADER_MAGIC,
	.flags = MULTIBOOT_HEADER_FLAGS,
	.checksum = (uint32_t)(-(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)),
	.mode_type = 0,
	.width = 1024,
	.height = 768,
	.depth = 32,
};

// 特别注意，这两个VMA=LMA
extern char boot_stack_top[]; // KERNEL_STACK_SIZE变了，要到kernel.ld改大小
extern uintptr_t boot_page_tables[];

uint32_t kernel_directory[PT_ENTRIES] __attribute__((aligned(PAGE_SIZE))) = {0};
static char boot_cmdline[BOOT_CMDLINE_SIZE] = {0};

void kernel_init(void);

__attribute__((fastcall)) // 避免参数压栈，在naked函数里不能这么做
static void copy_multiboot_info(multiboot_t *glb_mboot_ptr) {
	struct boot_info *p = (struct boot_info *)VMA2LMA(&boot_info);
	char *cmdline = (char *)VMA2LMA(boot_cmdline);

	if (glb_mboot_ptr->flags & MULTIBOOT_INFO_MEMORY) {
		p->mem_lower = glb_mboot_ptr->mem_lower;
		p->mem_upper = glb_mboot_ptr->mem_upper;
	}

	if (glb_mboot_ptr->flags & MULTIBOOT_INFO_CMDLINE) {
		const char *mboot_cmdline = (const char *)glb_mboot_ptr->cmdline;

		for (size_t i=0; i<BOOT_CMDLINE_SIZE-1; ++i) {
			cmdline[i] = mboot_cmdline[i];
			if (!mboot_cmdline[i])
				break;
		}

		p->cmdline = boot_cmdline;
	}

	if (glb_mboot_ptr->flags & MULTIBOOT_INFO_MODS) {
		if (glb_mboot_ptr->mods_count > 0) {
			struct multiboot_mod_list *mod = (struct multiboot_mod_list *)glb_mboot_ptr->mods_addr;
			p->initrd.start = mod->mod_start;
			p->initrd.end = mod->mod_end;
		}
	}

	if (glb_mboot_ptr->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) {
		p->framebuffer_type = glb_mboot_ptr->framebuffer_type;
		p->framebuffer_addr = glb_mboot_ptr->framebuffer_addr;
		p->framebuffer_pitch = glb_mboot_ptr->framebuffer_pitch;
		p->framebuffer_width = glb_mboot_ptr->framebuffer_width;
		p->framebuffer_height = glb_mboot_ptr->framebuffer_height;
		p->framebuffer_bpp = glb_mboot_ptr->framebuffer_bpp;
	}
}

static acpi_rsdptr_t *find_rsdp(uintptr_t ebda) {
	uint64_t magic = 0x2052545020445352ULL; // "RSD PTR "
	// The first 1 KB of the Extended BIOS Data Area (EBDA)
	for (uintptr_t addr=ebda; addr<ebda+1024; addr+=16)
	if (magic == *(uint64_t *)addr)
		return (acpi_rsdptr_t *)addr;

	// The BIOS read-only memory space between 0E0000h and 0FFFFFh
	for (uintptr_t addr=0xE0000; addr<0x100000; addr+=16)
	if (magic == *(uint64_t *)addr)
		return (acpi_rsdptr_t *)addr;

	return NULL;
}

static mmap_entry_t *find_region(multiboot_t *glb_mboot_ptr, uintptr_t rsdt) {
	uintptr_t start = glb_mboot_ptr->mmap_addr;
	uintptr_t end = glb_mboot_ptr->mmap_addr+glb_mboot_ptr->mmap_length;

	for (uintptr_t addr=start; addr<end; ) {
		mmap_entry_t *entry = (mmap_entry_t *)addr;
		if ((rsdt >= entry->addr) && (rsdt < (entry->addr + entry->len)))
			return entry;
		addr = addr + sizeof(entry->size) + entry->size;
	}

	return NULL;
}

__attribute__((fastcall))
static void find_acpi_region(multiboot_t *glb_mboot_ptr) {
	struct boot_info *p = (struct boot_info *)VMA2LMA(&boot_info);
	acpi_rsdptr_t *rsdp = find_rsdp(glb_mboot_ptr->mem_lower * 1024);
	if (rsdp == NULL)
		return;

	p->rsdt = rsdp->rsdt;

	mmap_entry_t *region = find_region(glb_mboot_ptr, rsdp->rsdt);
	if (region == NULL)
		return;

	p->acpi.start = region->addr;
	p->acpi.end = region->addr + region->len;
}

__attribute__((fastcall))
static void find_elf_sec(multiboot_t *glb_mboot_ptr) {
	struct boot_info *p = (struct boot_info *)VMA2LMA(&boot_info);
	size_t num = glb_mboot_ptr->u.elf_sec.num;
	size_t shndx = glb_mboot_ptr->u.elf_sec.shndx;
	elf_section_header_t *sh = (elf_section_header_t *)(glb_mboot_ptr->u.elf_sec.addr);
	const char *shstrtab = (const char *)sh[shndx].addr;
	for (size_t i=0; i<num; ++i) {
		const char *name = shstrtab + sh[i].name;
		if (strcmp(name, ".strtab") == 0) {
			p->strtab.start = sh[i].addr;
			p->strtab.end = sh[i].addr + sh[i].size;
		} else if (strcmp(name, ".symtab") == 0) {
			p->symtab.start = sh[i].addr;
			p->symtab.end = sh[i].addr + sh[i].size;
		}
	}
}


__attribute__((naked,noipa)) // naked不允许使用栈，编译器有时会无视naked内联别的函数，noipa阻止kernel_init内联进来
void start() {
	multiboot_t *glb_mboot_ptr;
	uint32_t magic;
	__asm__("cli" : "=b"(glb_mboot_ptr), "=a"(magic));
	// 现在还没有栈，不能调用函数
	__asm__("outb %b1, %w0" : : "d"(cmos_index), "a"(0x80)); // 关闭NMI

	// 还没开分页，必须用物理地址。注意boot_stack_top已经是LMA了
	__asm__("mov %0, %%esp" : : "r"(boot_stack_top));

	/*
	 * 内核无法决定boot loader把数据放哪里
	 * 开启分页后落到区间外边就访问不到了，可能会散落在不同区域，一个个map过来也麻烦
	 * 这里把需要用到的复制到内核的内存区域
	 */
	copy_multiboot_info(glb_mboot_ptr);

	/*
	 * ACPI格式对mmap并不友好，在地址那里是没有长度的
	 * 比如ACPICA发现长度不够，会再调用一次acpi_os_map_memory，这样写起来比较麻烦，而且对内核不友好
	 * 尽管ACPI标准没有规定数据必须放同一个区域内，应该也没有人会把这个分成多个区域
	*/
	find_acpi_region(glb_mboot_ptr);

	/* 在 GRUB 提供的 multiboot 信息中寻找 内核 ELF 格式所提取的字符串表和符号表 */
	find_elf_sec(glb_mboot_ptr);

	uintptr_t *page_directory = (uintptr_t *)VMA2LMA(kernel_directory);
	/* 自指页表 */
	page_directory[1023] = ((uintptr_t)page_directory)|PT_P|PT_W;

	uintptr_t kernel_base=(uintptr_t)__kernel_start;
	/*
	 * 开启分页后，程序还运行在LMA，所以还需要同时有和物理地址1:1的映射
	 * 直接用大页映射2G，内核大小变了也不用改这里
	 */
	for (uintptr_t addr=0; addr<kernel_base; addr+=HUGE_PAGE_SIZE)
		page_directory[addr>>HUGE_PAGE_SHIFT] = addr|PT_P|PT_W|PT_PS;

	/*
	 * boot_page_tables预留了内核态所有page directory共用的page table，从__kernel_start到KERNEL_STACK_BASE
	 * 不然后面某个进程申请了一个新的page table，还需要同时更新所有已经存在的其他page directory，容易出问题
	 */
	for (uintptr_t addr=kernel_base; addr<KERNEL_STACK_BASE; addr+=HUGE_PAGE_SIZE)
		page_directory[addr>>HUGE_PAGE_SHIFT] = (uintptr_t)(&boot_page_tables[(addr-kernel_base)>>PAGE_SHIFT])|PT_P|PT_W;

	/* boot阶段内核占据的地址空间 */
	for (uintptr_t addr=kernel_base; addr<(uintptr_t)__frame_infos_end; addr+=PAGE_SIZE)
		boot_page_tables[(addr-kernel_base)>>PAGE_SHIFT] = VMA2LMA(addr)|PT_P|PT_W;

	/* 内核地址空间在boot阶段没用到的部分 */
	/* 注意: kernel.ld里需要把__frame_infos_end设置成4K对齐的 */
	for (uintptr_t addr=(uintptr_t)__frame_infos_end; addr<KERNEL_STACK_BASE; addr+=PAGE_SIZE)
		boot_page_tables[((addr-kernel_base)>>PAGE_SHIFT)] = 0;

	/* boot进程的内核栈，从KERNEL_STACK_BASE到KERNEL_STACK_TOP，反正一时半会儿也回收不了就放这儿了 */
	for (uintptr_t addr=KERNEL_STACK_BASE; addr<KERNEL_STACK_TOP; addr+=HUGE_PAGE_SIZE)
		page_directory[addr>>HUGE_PAGE_SHIFT] = (uintptr_t)(&boot_page_tables[(addr-kernel_base)>>PAGE_SHIFT])|PT_P|PT_W;

	for (uintptr_t addr=KERNEL_STACK_TOP-KERNEL_STACK_SIZE; addr<KERNEL_STACK_TOP; addr+=PAGE_SIZE)
		boot_page_tables[(addr-kernel_base)>>PAGE_SHIFT] = (((uintptr_t)boot_stack_top)-(KERNEL_STACK_TOP-addr))|PT_P|PT_W;

	/* 把ESP设置成VMA */
	__asm__("mov %0, %%esp" : : "i"(KERNEL_STACK_TOP));
	__asm__("movl %0, %%cr4" : : "r"(0x10)); // CR4.PSE (大页)
	__asm__("movl %0, %%cr3" : : "r"(VMA2LMA(kernel_directory)));

	/*
	 * CR0.EM: 0x00000004  禁用FPU
	 * CR0.PE: 0x00000001  保护模式
	 * CR0.PG: 0x80000000  分页
	 */
	__asm__("movl %0, %%cr0" : : "r"(0x80000005));

	// 跳转到绝对地址
	__asm__("call *%0" : : "r"(kernel_init));
}
