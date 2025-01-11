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
#include "os_terminal.lib.h"
#include "sched.h"

static int caps_lock;
static int num_lock;
static int scroll_lock;

fifo_t terminal_key;

static struct ilist_node kb_proc_list;

/* 键盘中断处理 */
static void keyboard_handler(struct interrupt_frame *frame)
{
	uint8_t scan_code = inb(KB_DATA);
	const char *p = terminal_handle_keyboard(scan_code);
	if (p != 0) {
		while (*p != '\0') {
			fifo_put(&terminal_key, *p);
			p++;
		}
		queue_task_list(&kb_proc_list);
	}
}

/* 等待键盘控制器 */
static void kb_wait(void)
{
	uint8_t kb_stat;
	do {
		kb_stat = inb(KB_CMD);
	} while (kb_stat & 0x02);
}

/* 设置键盘LED */
static void set_leds(void)
{
	uint8_t leds = (caps_lock << 2) | (num_lock << 1) | scroll_lock;

	kb_wait();
	outb(KB_DATA, LED_CODE);

	kb_wait();
	outb(KB_DATA, leds);
}

/* 等待键盘传来的字符 */
void getch(char *ch)
{
	while (fifo_status(&terminal_key) == 0) {
		ilist_insert_before(&kb_proc_list, &(current->wait_list));
		yield();
	}
	*ch = fifo_get(&terminal_key);
}

/* 初始化键盘驱动器 */
void init_keyboard(void)
{
	print_busy("Initializing PS/2 keyboard controller...\r"); // 提示用户正在初始化键盘接口，并回到行首等待覆盖
	ilist_init(&kb_proc_list);
	uint32_t *term_buf = (uint32_t *)kmalloc(128);
	fifo_init(&terminal_key, 32, term_buf);

	caps_lock = 0;
	num_lock = 1;
	scroll_lock = 0;

	set_leds();
	register_interrupt_handler(0x21, &keyboard_handler);
	print_succ("PS/2 keyboard controller initialized successfully.\n");
}
