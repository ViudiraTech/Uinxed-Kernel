/*
 *
 *		main.c
 *		内核主入口
 *
 *		2024/6/23 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "idt.h"
#include "gdt.h"
#include "debug.h"
#include "printk.h"
#include "memory.h"
#include "keyboard.h"
#include "mouse.h"
#include "uinxed.h"
#include "pci.h"
#include "serial.h"
#include "parallel.h"
#include "ide.h"
#include "fdc.h"
#include "timer.h"
#include "beep.h"
#include "cpu.h"
#include "bochs.h"
#include "vbe.h"
#include "multiboot.h"
#include "task.h"
#include "fpu.h"
#include "sched.h"
#include "bmp.h"
#include "acpi.h"
#include "klogo.lib.h"
#include "os_terminal.lib.h"
#include "vdisk.h"
#include "devfs.h"
#include "fat.h"
#include "iso9660.h"
#include "list.h"
#include "vfs.h"
#include "file.h"
#include "syscall.h"
#include "tty.h"

void shell(const char *); // 声明shell程序入口

/* 内核shell进程 */
int kthread_shell(void *arg)
{
	char *cmdline = strchr((char *)arg, ' ');
	if (cmdline) ++cmdline;
	shell(cmdline);
	return 0;
}

/* 内核入口 */
void kernel_init(multiboot_t *glb_mboot_ptr)
{
	program_break_end = program_break + 0x300000 + 1 + KHEAP_INITIAL_SIZE;
	memset(program_break, 0, program_break_end - program_break);

	init_bochs(glb_mboot_ptr);						// 初始化bochs图形模式
	init_vbe(glb_mboot_ptr, 0x151515, 0xffffff);	// 初始化图形模式

	/* 检测内存是否达到最低要求 */
	if ((glb_mboot_ptr->mem_upper + glb_mboot_ptr->mem_lower) / 1024 + 1 < 32) {
		panic(P001);
	}

#ifdef SHOW_START_LOGO
	bmp_analysis((Bmp *)klogo, vbe_get_width() - 225, 25, 1);			// 显示内核Logo
#endif // SHOW_START_LOGO

	printk("Uinxed-Kernel "KERNL_VERS" (build-%d)\n", KERNL_BUID);		// 打印内核信息
	printk(PROJK_COPY"\n");												// 打印版权信息
	printk("This version compiles at "BUILD_DATE" "BUILD_TIME"\n\n");	// 打印编译日期时间

	printk("KernelArea: 0x00000000 - 0x%08X | GraphicsBuffer: 0x%08X\n", program_break_end,
                                                                         glb_mboot_ptr->framebuffer_addr);
	print_time("Initializing operating system kernel components.\n\n");	// 提示用户正在初始化内核

	init_gdt();						// 初始化GDT
	init_idt();						// 初始化IDT
	ISR_registe_Handle();			// 注册ISR处理
	acpi_init();					// 初始化ACPI
	init_page(glb_mboot_ptr);		// 初始化内存分页
	setup_free_page();				// 初始化用于页目录FIFO
	init_fpu();						// 初始化FPU
	init_pci();						// 初始化PCI设备
	init_serial(9600);				// 初始化计算机串口
	init_parallel();				// 初始化计算机并口LPT
	init_keyboard();				// 初始化键盘驱动
	init_tty();						// 初始化电传打字设备
	mouse_init();					// 初始化鼠标驱动
	init_sched();					// 初始化多任务
	syscall_init();					// 初始化系统调用
	floppy_init();					// 初始化软盘控制器
	init_ide();						// 初始化IDE

	vbe_write_newline();			// 打印一个空行，和上面的信息保持隔离

	vfs_init();						// 初始化虚拟文件系统
	devfs_regist();					// 注册devfs
	fatfs_regist();					// 注册fatfs
	iso9660_regist();				// 注册iso9660
	file_init();					// 文件操作抽象层初始化
	if (vfs_do_search(vfs_open("/dev"), "hda")) {
		vfs_mount("/dev/hda", vfs_open("/"));
		print_succ("Root filesystem mounted ('/dev/hda' -> '/')\n");
	} else {
		print_warn("The root file system could not be mounted.\n");
	}
	init_timer(1);					// 初始化定时器
	init_pit();						// 初始化PIT

	terminal_set_color_scheme(3);	// 重置终端主题

	enable_intr();					// 开启中断
	enable_scheduler();				// 启用调度

	vbe_write_newline();
	print_time("The kernel components of the operating system are initialized.\n");

	system_beep(1000);				// 初始化完毕后蜂鸣
	sleep(10);
	system_beep(0);

	vbe_write_newline();			// 打印一个空行，和上面的信息保持隔离
	print_cpu_info();				// 打印当前CPU的信息

	printk("Terminal Uinxed %s\n", get_boot_tty());

#ifdef DEBUG_SHELL
	kernel_thread(kthread_shell, (void *)((glb_mboot_ptr->flags & MULTIBOOT_INFO_CMDLINE) ? glb_mboot_ptr->cmdline : 0),
                  "Basic shell program", USER_TASK);
#else
	if (vfs_do_search(vfs_open("/dev"), "hda")) {
		if (vfs_do_search(vfs_open("/"), "sbin")) {
			if (vfs_do_search(vfs_open("/sbin"), "init")) {
				elf_thread("/sbin/init", 0, "init", USER_TASK);
			} else {
				panic("No working init found.");
			}
		} else {
			panic("No working init found.");
		}
	} else {
		panic("VFS: Unable to mount root fs on unkown-block(0,0)");
	}
#endif // DEBUG_SHELL

	/* 内核运行时 */
	terminal_set_auto_flush(0);
	while (1) {
		/* OST刷新 */
		uint32_t eflags = load_eflags();
		if (eflags & (1 << 9)) disable_intr();
		terminal_flush();
		if (eflags & (1 << 9)) enable_intr();

		/* 释放FIFO中的页目录 */
		free_pages();

		/* 停机一次 */
		__asm__ __volatile__("hlt");
	}
}
