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
#include "ide.h"
#include "timer.h"
#include "beep.h"
#include "cpu.h"
#include "vbe.h"
#include "multiboot.h"
#include "task.h"
#include "fpu.h"
#include "sched.h"
#include "bmp.h"
#include "acpi.h"
#include "klogo.lib.h"
#include "lib_os_terminal.lib.h"
#include "vdisk.h"
#include "devfs.h"
#include "fat.h"
#include "list.h"
#include "vfs.h"
#include "file.h"

void shell(void); // 声明shell程序入口

/* 内核shell进程 */
int kthread_shell(void *arg)
{
	shell();
	return 0;
}

/* Terminal手动刷新线程 */
int terminal_manual_flush(void *arg)
{
	terminal_set_auto_flush(0);
	while (1) {
		uint32_t eflags = load_eflags();
		if (eflags & (1 << 9)) disable_intr();
		terminal_flush();
		if (eflags & (1 << 9)) enable_intr();
		asm volatile("hlt");
	}
}

/* 内核入口 */
void kernel_init(multiboot_t *glb_mboot_ptr)
{
	vbe_to_serial(1);								// 输出内核启动日志到串口
	init_vbe(glb_mboot_ptr, 0x151515, 0xffffff);	// 初始化图形模式

	/* 检测内存是否达到最低要求 */
	if ((glb_mboot_ptr->mem_upper + glb_mboot_ptr->mem_lower) / 1024 + 1 < 16) {
		panic(P001);
	}
	bmp_analysis((Bmp *)klogo, 799, 25, 1);								// 显示内核Logo
	printk("Uinxed-Kernel "KERNL_VERS" (build-%d)\n", KERNL_BUID);		// 打印内核信息
	printk(PROJK_COPY"\n");												// 打印版权信息
	printk("This version compiles at "BUILD_DATE" "BUILD_TIME"\n\n");	// 打印编译日期时间

	printk("Initializing operating system kernel components.\n");		// 提示用户正在初始化内核

	init_gdt();						// 初始化GDT
	init_idt();						// 初始化IDT
	ISR_registe_Handle();			// 注册ISR处理
	acpi_init();					// 初始化ACPI
	init_page(glb_mboot_ptr);		// 初始化内存分页
	init_fpu();						// 初始化FPU
	init_pci();						// 初始化PCI设备
	init_serial();					// 初始化计算机串口
	init_keyboard();				// 初始化键盘驱动
	mouse_init();					// 初始化鼠标驱动
	init_sched();					// 初始化多任务
	init_ide();						// 初始化IDE

	vbe_write_newline();			// 打印一个空行，和上面的信息保持隔离

	vfs_init();						// 初始化虚拟文件系统
	devfs_regist();					// 注册devfs
	fatfs_regist();					// 注册fatfs
	file_init();					// 文件操作抽象层初始化
	if (vfs_do_search(vfs_open("/dev"), "sda")) {
		vfs_mount("/dev/sda", vfs_open("/"));
		print_succ("Root filesystem mounted ('/dev/sda' -> '/')\n");
	} else {
		print_warn("The root file system could not be mounted.\n");
	}

	init_timer(1);					// 初始化定时器
	init_pit();						// 初始化PIT

	enable_intr();					// 开启中断

	system_beep(1000);				// 初始化完毕后蜂鸣
	sleep(10);
	system_beep(0);

	vbe_write_newline();			// 打印一个空行，和上面的信息保持隔离
	print_cpu_info();				// 打印当前CPU的信息

	terminal_set_color_scheme(0);	// 重置终端主题
	vbe_to_serial(0);				// 停止输出内核启动日志到串口

	kernel_thread(kthread_shell, 0, "Basic shell program", USER_TASK);
	kernel_thread(terminal_manual_flush, 0, "Terminal manual flush", SERVICE_TASK);
}
