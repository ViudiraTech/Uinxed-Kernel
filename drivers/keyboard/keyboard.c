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
#include "math.h"

static int code_with_E0 = 0;
static int shift_l;
static int shift_r;
static int alt_l;
static int alt_r;
static int ctrl_l;
static int ctrl_r;
static int caps_lock;
static int num_lock;
static int scroll_lock;
static int column;

static void keyboard_read(void);

fifo_t keyfifo, decoded_key;
extern uint32_t keymap[];

void keyboard_handler(pt_regs *regs)
{
	uint8_t c = inb(KB_DATA);
	fifo_put(&keyfifo, c);
	keyboard_read();
}

static void kb_wait(void)
{
	uint8_t kb_stat;

	do {
		kb_stat = inb(KB_CMD);
	} while (kb_stat & 0x02);
}

/* 此函数会检查是否收到PS/2键盘的ACK信号，如果没有收到ACK信号就会一直循环卡住。
 * 现代计算机已经可以将USB键盘模拟为PS/2键盘了，所以没必要用此函数检查了。
static void kb_ack(void)
{
	uint8_t kb_read;

	do {
		kb_read = inb(KB_DATA);
	} while (kb_read != KB_ACK);
}
*/

static void set_leds(void)
{
	uint8_t leds = (caps_lock << 2) | (num_lock << 1) | scroll_lock;

	kb_wait();
	outb(KB_DATA, LED_CODE);
	// kb_ack();

	kb_wait();
	outb(KB_DATA, leds);
	// kb_ack();
}

void init_keyboard(void)
{
	print_busy("Initializing PS/2 keyboard controller...\r"); // 提示用户正在初始化键盘接口，并回到行首等待覆盖
	uint32_t *keybuf = (uint32_t *)kmalloc(128);
	uint32_t *dkey_buf = (uint32_t *)kmalloc(128);
	fifo_init(&keyfifo, 32, keybuf);
	fifo_init(&decoded_key, 32, dkey_buf);

	shift_l = shift_r = 0;
	alt_l = alt_r = 0;
	ctrl_l = ctrl_r = 0;

	caps_lock = 0;
	num_lock = 1;
	scroll_lock = 0;

	set_leds();
	register_interrupt_handler(IRQ1, &keyboard_handler);
	print_succ("PS/2 keyboard controller initialized successfully.\n");
}

static uint8_t get_scancode(void)
{
	uint8_t scan_code;
	disable_intr();
	scan_code = fifo_get(&keyfifo);
	return scan_code;
}

static void in_process(uint32_t key)
{
	if (!(key & FLAG_EXT)) {
		fifo_put(&decoded_key, key);
	} else {
		int raw_code = key & MASK_RAW;
		switch (raw_code) {
			case ENTER:
				fifo_put(&decoded_key, '\n');
				break;
			case BACKSPACE:
				fifo_put(&decoded_key, '\b');
				fifo_put(&decoded_key, ' ');
				fifo_put(&decoded_key, '\b');
				break;
			case LEFT:
			case UP:
			case RIGHT:
			case TAB:
				fifo_put(&decoded_key, key);
				break;
		}
	}
}

static bool enable = true;

static uint8_t get_scancode_utill(void)
{
	enable = false;
	while (fifo_status(&keyfifo) == 0) {
		enable_intr();
	}
	disable_intr();
	uint8_t r = get_scancode();
	enable = true;
	return r;
}

static void keyboard_read(void)
{
	uint8_t scan_code;
	int make;

	uint32_t key = 0;
	uint32_t *keyrow;

	if (!enable)
		return;
	if (fifo_status(&keyfifo) > 0) {
		code_with_E0 = 0;
		scan_code = get_scancode();
		if (scan_code == 0xE1) {
			int i;
			uint8_t pausebrk_scode[] = {0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5};
			int is_pausebreak = 1;
			for (i = 1; i <= 6; i++) {
				if (get_scancode() != pausebrk_scode[i]) {
					is_pausebreak = 0;
					break;
				}
			}
			if (is_pausebreak)
			key = PAUSEBREAK;
		} else if (scan_code == 0xE0) {
			scan_code = get_scancode_utill();
			if (scan_code == 0x2A) {
				if (get_scancode_utill() == 0xE0) {
					if (get_scancode_utill() == 0x37) {
						key = PRINTSCREEN;
						make = 1;
					}
				}
			}
			if (scan_code == 0xB7) {
				if (get_scancode_utill() == 0xE0) {
					if (get_scancode_utill() == 0xAA) {
						key = PRINTSCREEN;
						make = 0;
					}
				}
			}
			if (scan_code == 0x48) { // ↑
				key = UP;
				in_process(key);
				return;
			} else if (scan_code == 0x50) { // ↓
				key = DOWN;
				in_process(key);
				return;
			} else if (scan_code == 0x4b) { // ←
				key = LEFT;
				in_process(key);
				return;
			} else if (scan_code == 0x4d) { // →
				key = RIGHT;
				in_process(key);
				return;
			}
			if (key == 0) {
				code_with_E0 = 1;
			}
		}
		if ((key != PAUSEBREAK) && (key != PRINTSCREEN) && (key != RIGHT) &&
            (key != LEFT) && (key != UP) && (key != RIGHT)) {
			make = (scan_code & FLAG_BREAK ? false : true);
			keyrow = &keymap[(scan_code & 0x7F) * MAP_COLS];
			column = 0;

			int caps = shift_l || shift_r;
			if (caps_lock) {
				if ((keyrow[0] >= 'a') && (keyrow[0] <= 'z')) {
					caps = !caps;
				}
			}
			if (caps) {
				column = 1;
			}
			if (code_with_E0) {
				column = 2;
			}
			key = keyrow[column];

			switch (key) {
				case SHIFT_L:
					shift_l = make;
					break;
				case SHIFT_R:
					shift_r = make;
					break;
				case CTRL_L:
					ctrl_l = make;
					break;
				case CTRL_R:
					ctrl_r = make;
					break;
				case ALT_L:
					alt_l = make;
					break;
				case ALT_R:
					alt_r = make;
					break;
				case CAPS_LOCK:
					if (make) {
						caps_lock = !caps_lock;
						set_leds();
					}
					break;
				case NUM_LOCK:
					if (make) {
						num_lock = !num_lock;
						set_leds();
					}
					break;
				case SCROLL_LOCK:
					if (make) {
						scroll_lock = !scroll_lock;
						set_leds();
					}
					break;
				default:
					break;
			}
			if (make) {
				int pad = 0;

				if ((key >= PAD_SLASH) && (key <= PAD_9)) {
					pad = 1;
					switch (key) {
						case PAD_SLASH:
							key = '/';
							break;
						case PAD_STAR:
							key = '*';
							break;
						case PAD_MINUS:
							key = '-';
							break;
						case PAD_PLUS:
							key = '+';
							break;
						case PAD_ENTER:
							key = ENTER;
							break;
						default:
							if (num_lock && (key >= PAD_0) && (key <= PAD_9)) {
								key = key - PAD_0 + '0';
							} else if (num_lock && (key == PAD_DOT)) {
								key = '.';
							} else {
								switch (key) {
									case PAD_HOME:
										key = HOME;
										break;
									case PAD_END:
										key = END;
										break;
									case PAD_PAGEUP:
										key = PAGEUP;
										break;
									case PAD_PAGEDOWN:
										key = PAD_PAGEDOWN;
										break;
									case PAD_INS:
										key = INSERT;
										break;
									case PAD_UP:
										key = UP;
										break;
									case PAD_DOWN:
										key = DOWN;
										break;
									case PAD_LEFT:
										key = LEFT;
										break;
									case PAD_RIGHT:
										key = RIGHT;
										break;
									case PAD_DOT:
										key = DELETE;
										break;
									default:
										break;
								}
							}
							break;
					}
				}
				key |= shift_l ? FLAG_SHIFT_L : 0;
				key |= shift_r ? FLAG_SHIFT_R : 0;
				key |= ctrl_l ? FLAG_CTRL_L : 0;
				key |= ctrl_r ? FLAG_CTRL_R : 0;
				key |= alt_l ? FLAG_ALT_L : 0;
				key |= alt_r ? FLAG_ALT_R : 0;
				key |= pad ? FLAG_PAD : 0;

				in_process(key);
			}
		}
	}
}
