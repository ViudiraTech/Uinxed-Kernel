/*
 *
 *		tty.c
 *		终端设备
 *
 *		2024/12/17 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "tty.h"
#include "multiboot.h"
#include "stdlib.h"
#include "memory.h"
#include "serial.h"
#include "common.h"
#include "os_terminal.lib.h"

int default_tty = 1;

static size_t u8strlen(uint8_t *str)
{
	size_t length = 0;
	while (str[length] != '\0') {
		length++;
	}
	return length;
}

static int arg_parse(uint8_t *arg_str, uint8_t **argv, uint8_t token)
{
	int arg_idx = 0;

	while (arg_idx < 32768) {
		argv[arg_idx] = 0;
		arg_idx++;
	}
	uint8_t *next = arg_str;
	int argc = 0;

	while (*next) {
		while (*next == token) next++;
		if (*next == 0) break;
		argv[argc] = next;
		while (*next && *next != token) next++;
		if (*next) {
			*next++ = 0;
		}
		if (argc > 32768) return 1;
		argc++;
	}
	return argc;
}

/* 获取启动时传入的tty号 */
int get_boot_tty(void)
{
	uint8_t *arg_based;
	arg_based = (unsigned char *)glb_mboot_ptr->cmdline;

	int i = 0;
	uint8_t bootarg[256] = {0};

	while (arg_based[i] != '\0') {
		bootarg[i] = arg_based[i];
		i++;
	}
	uint8_t **bootargv = (uint8_t **)kmalloc(32768 * sizeof(uint8_t *));
	if (!bootargv)return 1;

	int argc = arg_parse(bootarg, bootargv, ' ');

	for (int j = 0; j < argc; j++) {
		if (strncmp((char *)bootargv[j], "tty=", 4) == 0) {
			uint8_t *tty_num_str = bootargv[j] + 4;
			int tty_num_len = u8strlen(tty_num_str);
			if (tty_num_len == 1 || tty_num_len == 2) {
				int tty_num = atoi((char *)tty_num_str);
				kfree(bootargv);
				return tty_num;
			}
		}
	}
	kfree(bootargv);
	return 1;
}

/* 打印日志字符到TTY */
void tty_print_logch(const char ch)
{
	uint32_t eflags = load_eflags();
	if (get_boot_tty() == 0) {
		write_serial(ch);
	} else if (get_boot_tty() == 1) {
		if (eflags & (1 << 9)) disable_intr();
		terminal_process_char(ch);
		if (eflags & (1 << 9)) enable_intr();
	}
}

/* 打印日志字符串到TTY */
void tty_print_logstr(const char *str)
{
	uint32_t eflags = load_eflags();
	if (get_boot_tty() == 0) {
		write_serial_string(str);
	} else if (get_boot_tty() == 1) {
		if (eflags & (1 << 9)) disable_intr();
		terminal_process(str);
		if (eflags & (1 << 9)) enable_intr();
	}
}
