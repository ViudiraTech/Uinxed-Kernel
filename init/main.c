/*
 *
 *		main.c
 *		Uinxed内核入口
 *
 *		2024/6/23 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "stdint.h"
#include "stddef.h"
#include "limine.h"
#include "common.h"
#include "video.h"
#include "printk.h"
#include "gdt.h"
#include "idt.h"
#include "uinxed.h"
#include "hhdm.h"
#include "frame.h"
#include "heap.h"
#include "page.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "interrupt.h"
#include "serial.h"
#include "smbios.h"

/* 内核入口 */
void kernel_entry(void)
{
	video_init();			// 初始化视频驱动

	plogk("Uinxed version %s (%s version %s) #1 SMP %s %s\n",
          KERNL_VERS, COMPILER_NAME, COMPILER_VERSION, BUILD_DATE, BUILD_TIME);
	plogk("Video memory address: 0x%08x | Resolution: %dx%d\n",
          get_framebuffer()->address, get_framebuffer()->width, get_framebuffer()->height);
	plogk("SMBIOS %d.%d.0 present.\n",smbios_major_version(), smbios_minor_version());

	init_hhdm();			// 初始化高半区内存映射
	init_frame();			// 初始化内存帧
	init_heap();			// 初始化内存堆
	init_gdt();				// 初始化全局描述符
	init_idt();				// 初始化中断描述符
	ISR_registe_Handle();	// 注册ISR中断处理
	page_init();			// 初始化内存页
	acpi_init();			// 初始化ACPI
	init_serial(9600);		// 初始化串口
	enable_intr();

	plogk("CPU: %s %s | phy/virt: %d/%d bits.\n", get_vendor_name(), get_model_name(), get_cpu_phys_bits(), get_cpu_virt_bits());
	while (1) __asm__("hlt");
}
