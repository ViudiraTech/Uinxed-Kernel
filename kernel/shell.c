/*
 *
 *		shell.c
 *		内核自带的shell交互程序
 *
 *		2024/7/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "fifo.h"
#include "keyboard.h"
#include "console.h"
#include "debug.h"
#include "types.h"
#include "string.h"
#include "cmos.h"
#include "printk.h"

#define MAX_COMMAND_LEN	100
#define MAX_ARG_NR		30

/* 从FIFO中读取一个按键事件 */
static int read_key_blocking(char *buf)
{
	while (fifo_status(&decoded_key) == 0);
	*buf++ = fifo_get(&decoded_key);
	return 0;
}

/* 解析命令行字符串 */
static int cmd_parse(uint8_t *cmd_str, uint8_t **argv, uint8_t token) // 用uint8_t是因为" "使用8位整数
{
	int arg_idx = 0;

	while (arg_idx < MAX_ARG_NR) {
		argv[arg_idx] = NULL;
		arg_idx++;
	}

	uint8_t *next = cmd_str;					// 下一个字符
	int	argc = 0;								// 这就是要返回的argc了

	while (*next) {								// 循环到结束为止
		while (*next == token) next++;			// 多个token就只保留第一个，windows cmd就是这么处理的
		if (*next == 0) break;					// 如果跳过完token之后结束了，那就直接退出
		argv[argc] = next;						// 将首指针赋值过去，从这里开始就是当前参数
		while (*next && *next != token) next++;	// 跳到下一个token
		if (*next) {							// 如果这里有token字符
			*next++ = 0;						// 将当前token字符设为0（结束符），next后移一个
		}
		if (argc > MAX_ARG_NR) return -1;		// 参数太多，超过上限了
		argc++; // argc增一，如果最后一个字符是空格时不提前退出，argc会错误地被多加1
	}
	return argc;
}

/* 读取一行文本 */
static void readline(char *buf, int cnt, int prompt_len)
{
	char *pos = buf;
	int buf_idx = 0;

	while (read_key_blocking(pos) != -1 && buf_idx < cnt) { // 没打够字符并且不出错
		switch (*pos) {
			case '\n':
			case '\r':
				*pos = 0; // 读完了，好耶！
				printk("\n");
				return;
			case '\b':
				if (buf[0] != '\b') {	// 哥们怎么全删完了
					--pos;				// 退回上一个字符
					--buf_idx;			// 更新字符索引
					printk("\b");		// 打印一个退格
				}
				break;
			default:
				/* 其他 */
				printk("%c", *pos);
				pos++;
				if (buf_idx - prompt_len < cnt) { // 确保不超过输入限制
					buf_idx++;
				}
		}
	}
}

/* help命令 */
void shell_help(int argc, char *argv)
{
	printk("+---------+------------------------------+\n"
           "| Command |   Command description        |\n"
           "+---------+------------------------------+\n"
           "|  help   |  Show help like this.        |\n"
           "|  clear  |  Clear the screen.           |\n"
           "|  cpuid  |  List for CPU information.   |\n"
           "+---------+------------------------------+\n\n");
	return;
}

typedef struct builtin_cmd
{
	char *name;
	void (*func)(int argc);
} builtin_cmd_t;

builtin_cmd_t builtin_cmds[] = {
	{"clear", console_clear},
	{"help", shell_help},
	{"cpuid", print_cpu_id}
};

/* 内建命令数量 */
const static int builtin_cmd_num = sizeof(builtin_cmds) / sizeof(builtin_cmd_t);

/* 在预定义的命令数组中查找给定的命令字符串 */
int find_cmd(char *cmd)
{
	for (int i = 0; i < builtin_cmd_num; ++i)
	{
		if (strcmp(cmd, builtin_cmds[i].name) == 0){
			return i;
		}
	}
	return -1;
}

/* shell主程序 */
void shell(void)
{
	printk("Basic shell program v1.0\n");
	printk("Type 'help' for help.\n\n");

	char *prompt_len = "Shell-[Kernel:Ring0]-# ";			// 提示符，可自行修改
	uint8_t cmd[MAX_COMMAND_LEN];
	uint8_t *argv[MAX_ARG_NR];
	int argc = -1;

	while (true) {
		printk(prompt_len);									// 打印提示符

		memset(cmd, 0, MAX_COMMAND_LEN);					// 清空上轮输入
		readline(cmd, MAX_COMMAND_LEN, strlen(prompt_len));	// 读入一行

		/* com就是完整的命令 */
		if (cmd[0] == 0) continue;							// 只有一个回车，continue
		argc = cmd_parse(cmd, argv, ' ');

		/* argc, argv 都拿到了 */
		if (argc == -1) {
			print_erro("shell: number of arguments exceed MAX_ARG_NR(30)");
			continue;
		} else if (argc == 0) {
			console_write_newline();
			continue;
		}

		int cmd_index = find_cmd(argv[0]);
		if (cmd_index < 0) {
			/* 找不到该命令 */
			printk("Command not found.\n\n");
		} else {
			builtin_cmds[cmd_index].func(argc);
		}
	}
}
