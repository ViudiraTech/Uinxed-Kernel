/*
 *
 *		tty.c
 *		Teletype
 *
 *		2025/4/12 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "alloc.h"
#include "cmdline.h"
#include "serial.h"
#include "stdint.h"
#include "string.h"
#include "video.h"

/* Argument parsing */
static int arg_parse(char *arg_str, char **argv, char token)
{
	int	  argc	  = 0;
	int	  arg_idx = 0;
	char *next	  = arg_str;

	while (arg_idx < 32768) {
		argv[arg_idx] = 0;
		arg_idx++;
	}
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

/* Obtain the tty number provided at startup */
const char *get_boot_tty(void)
{
	int			i			 = 0;
	char		bootarg[256] = {0};
	const char *arg_based	 = get_cmdline();

	while (arg_based[i] != '\0') {
		bootarg[i] = arg_based[i];
		i++;
	}
	char **bootargv = malloc(32768 * sizeof(char *));
	if (!bootargv) return "tty0";

	int argc = arg_parse(bootarg, bootargv, ' ');

	for (int j = 0; j < argc; j++) {
		if (strncmp(bootargv[j], "console=", 8) == 0) {
			const char *tty_str		= bootargv[j] + 8;
			int			tty_num_len = strlen(tty_str);
			if (tty_num_len == 1 || tty_num_len == 5) {
				free(bootargv);
				return tty_str;
			}
		}
	}
	free(bootargv);
	return "tty0";
}

/* Print characters to tty */
void tty_print_ch(const char ch)
{
	if (strcmp(get_boot_tty(), "ttyS0") == 0)
		write_serial(ch);
	else
		video_put_string((char[]){ch, '\0'});
}

/* Print string to tty */
void tty_print_str(const char *str)
{
	if (strcmp(get_boot_tty(), "ttyS0") == 0)
		for (; *str; ++str) write_serial(*str);
	else
		video_put_string(str);
}
