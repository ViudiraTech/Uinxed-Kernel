/*
 *
 *		pl_readline.lib.h
 *		pl_readline库头文件
 *
 *		2024/10/20 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_PL_READLINE_LIB_H_
#define INCLUDE_PL_READLINE_LIB_H_

#include "types.h"
#include "list.h"

#define PL_READLINE_KEY_UP			0xff00
#define PL_READLINE_KEY_DOWN		0xff01
#define PL_READLINE_KEY_LEFT		0xff02
#define PL_READLINE_KEY_RIGHT		0xff03
#define PL_READLINE_KEY_HOME		0xff04
#define PL_READLINE_KEY_END			0xff05
#define PL_READLINE_KEY_PAGE_UP		0xff06
#define PL_READLINE_KEY_PAGE_DOWN	0xff07
#define PL_READLINE_KEY_ENTER		'\n'
#define PL_READLINE_KEY_TAB			'\t'
#define PL_READLINE_KEY_BACKSPACE	'\b'
#define PL_READLINE_SUCCESS			0
#define PL_READLINE_FAILED			-1
#define PL_READLINE_NOT_FINISHED	1

typedef struct pl_readline_word {
	char *word;	// 词组
	bool first;
	char sep;	// 分隔符
} pl_readline_word;

typedef struct pl_readline_words {
	int len;					// 词组数量
	int max_len;				// 词组最大数量
	pl_readline_word *words;	// 词组列表
} *pl_readline_words_t;

typedef struct pl_readline {
	int (*pl_readline_hal_getch)(void);										// 输入函数
	void (*pl_readline_hal_putch)(int ch);									// 输出函数
	void (*pl_readline_hal_flush)(void);									// 刷新函数
	void (*pl_readline_get_words)(char *buf, pl_readline_words_t words);	// 获取词组列表
	list_t history;															// 历史记录列表
} *pl_readline_t;

typedef struct pl_readline_runtime {
	char *buffer;				// 输入缓冲区
	int p;						// 输入缓冲区指针
	int length;					// 输入缓冲区长度（已经输入的字符数）
	int history_idx;			// 历史记录指针
	char *prompt;				// 提示符
	size_t len;					// 缓冲区最大长度
	char *input_buf;			// 输入缓冲区（补全的前缀）
	int input_buf_ptr;			// 输入缓冲区（补全的前缀）指针
	bool intellisense_mode;		// 智能补全模式
	char *intellisense_word;	// 智能补全词组
} pl_readline_runtime;

extern pl_readline_words_t pl_readline_word_maker_init(void);
extern int pl_readline(pl_readline_t self, const char *prompt, char *buffer, size_t len);
extern void pl_readline_insert_char_and_view(pl_readline_t self, char ch, pl_readline_runtime *rt);
extern void pl_readline_insert_char(char *str, char ch, int idx);
extern int pl_readline_word_maker_add(const char *word, pl_readline_words_t words, bool is_first, char sep);
extern void pl_readline_print(pl_readline_t self, char *str);
extern void pl_readline_intellisense_insert(pl_readline_t self, pl_readline_runtime *rt, pl_readline_word words);
extern void pl_readline_word_maker_destroy(pl_readline_words_t words);
extern void pl_readline_next_line(pl_readline_t self, pl_readline_runtime *rt);
extern int pl_readline_handle_key(pl_readline_t self, int ch, pl_readline_runtime *rt);
extern void pl_readline_uninit(pl_readline_t self);
extern pl_readline_word pl_readline_intellisense(pl_readline_t self, pl_readline_runtime *rt, pl_readline_words_t words);
extern pl_readline_t pl_readline_init(int (*pl_readline_hal_getch)(void), void (*pl_readline_hal_putch)(int ch),
                                      void (*pl_readline_hal_flush)(void), void (*pl_readline_get_words)(char *buf, pl_readline_words_t words));

#endif // INCLUDE_PL_READLINE_LIB_H_
