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
#include "vdisk.h"
#include "keyboard.h"
#include "os_terminal.lib.h"

int default_tty = 1;

/* 传递给vdisk的读接口 */
static void vdisk_tty0_read(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	*buffer = fifo_get(&terminal_key);
}

/* 传递给vdisk的写接口 */
static void vdisk_tty0_write(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	tty_print_logstr((const char *)buffer);
}

static unsigned int u8strlen(char *str)
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
char *get_boot_tty(void)
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
	if (!bootargv) return (char *)"tty0";

	int argc = arg_parse(bootarg, bootargv, ' ');

	for (int j = 0; j < argc; j++) {
		if (strncmp((char *)bootargv[j], "console=", 8) == 0) {
			char *tty_str = (char *)bootargv[j] + 8;
			int tty_num_len = u8strlen(tty_str);
			if (tty_num_len == 1 || tty_num_len == 5) {
				kfree(bootargv);
				return tty_str;
			}
		}
	}
	kfree(bootargv);
	return (char *)"tty0";
}

/* 打印日志字符到TTY */
void tty_print_logch(const char ch)
{
	if (strcmp(get_boot_tty(), "ttyS0") == 0) {
		write_serial(ch);
	} else if (strcmp(get_boot_tty(), "tty0") == 0) {
		terminal_process_char(ch);
	}
}

/* 打印日志字符串到TTY */
void tty_print_logstr(const char *str)
{
	if (strcmp(get_boot_tty(), "ttyS0") == 0) {
		write_serial_string(str);
	} else if (strcmp(get_boot_tty(), "tty0") == 0) {
		terminal_process(str);
	}
}

/* 初始化tty设备 */
void init_tty(void)
{
	print_busy("Initializing teletype device...\r"); // 提示用户正在初始化tty设备，并回到行首等待覆盖

	/* 注册到vdisk */
	vdisk vd;
	vd.flag = 1;
	vd.Read = vdisk_tty0_read;
	vd.Write = vdisk_tty0_write;
	vd.sector_size = 1;
	vd.size = 1;
	sprintf(vd.DriveName,"tty0");
	register_vdisk(vd);

	print_succ("Teletype device initialized successfully.\n");
}
