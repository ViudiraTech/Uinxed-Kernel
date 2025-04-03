/*
 *
 *		main.c
 *		Uinxed kernel entry
 *
 *		2024/6/23 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
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
#include "pci.h"
#include "cmdline.h"
#include "ide.h"

/* Kernel entry */
void kernel_entry(void)
{
	video_init();			// Initialize Video

	plogk("Uinxed version %s (%s version %s) #1 SMP %s %s\n",
          KERNL_VERS, COMPILER_NAME, COMPILER_VERSION, BUILD_DATE, BUILD_TIME);
	plogk("Framebuffer address at 0x%016x, resolution: %dx%d\n",
          get_framebuffer()->address, get_framebuffer()->width, get_framebuffer()->height);
	plogk("Command line: %s\n", get_cmdline());
	plogk("SMBIOS %d.%d.0 present.\n",smbios_major_version(), smbios_minor_version());

	init_gdt();				// Initialize global descriptors
	init_idt();				// Initialize interrupt descriptor
	init_frame();			// Initialize memory frame
	page_init();			// Initialize memory page
	init_hhdm();			// Initialize the upper half memory mapping
	init_heap();			// Initialize the memory heap
	acpi_init();			// Initialize ACPI
	isr_registe_handle();	// Register ISR interrupt processing
	pci_init();				// Initialize PCI
	init_serial(9600);		// Initialize the serial port
	init_ide();				// Initialize ATA/ATAPI driver
	enable_intr();

	plogk("CPU: %s %s\n", get_vendor_name(), get_model_name());
	plogk("CPU: phy/virt: %d/%d bits.\n", get_cpu_phys_bits(), get_cpu_virt_bits());
	while (1) __asm__("hlt");
}
