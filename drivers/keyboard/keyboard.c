/*
 *
 *		keyboard.c
 *		键盘驱动
 *
 *		2024/2/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "keyboard.h"
#include "common.h"
#include "fifo.h"
#include "idt.h"
#include "memory.h"
#include "printk.h"
#include "lib_os_terminal.lib.h"

static int caps_lock;
static int num_lock;
static int scroll_lock;

fifo_t terminal_key;

void keyboard_handler(pt_regs *regs)
{
	uint8_t scan_code = inb(KB_DATA);
	char buffer[8] = {0};
	disable_intr();
	if (terminal_handle_keyboard(scan_code, buffer)) {
		for (int i = 0; buffer[i]; i++) {
			fifo_put(&terminal_key, buffer[i]);
		}
	}
}

static void kb_wait(void)
{
	uint8_t kb_stat;
	do {
		kb_stat = inb(KB_CMD);
	} while (kb_stat & 0x02);
}

static void set_leds(void)
{
	uint8_t leds = (caps_lock << 2) | (num_lock << 1) | scroll_lock;

	kb_wait();
	outb(KB_DATA, LED_CODE);

	kb_wait();
	outb(KB_DATA, leds);
}

void init_keyboard(void)
{
	print_busy("Initializing PS/2 keyboard controller...\r"); // 提示用户正在初始化键盘接口，并回到行首等待覆盖
	uint32_t *term_buf = (uint32_t *)kmalloc(128);
	fifo_init(&terminal_key, 32, term_buf);

	caps_lock = 0;
	num_lock = 1;
	scroll_lock = 0;

	set_leds();
	register_interrupt_handler(IRQ1, &keyboard_handler);
	print_succ("PS/2 keyboard controller initialized successfully.\n");
}
