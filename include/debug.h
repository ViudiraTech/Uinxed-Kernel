/*
 *
 *		debug.h
 *		内核调试程序头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_DEBUG_H_
#define INCLUDE_DEBUG_H_

#include "types.h"

#define assert(exp) \
	if (!exp) assertion_failure(#exp, __FILE__, __LINE__)

#define assertx(x, info) \
	if (!x) panic(info);

#define PFFF "UNKNOWN_ERROR"
#define P000 "TEST_KERNEL_PANIC"
#define P001 "OUT_OF_MEMORY"
#define P002 "FRAMES_FREE_ERROR-CannotFreeFrames"
#define P003 "PAGE_FAULT-Present-Address:"
#define P004 "PAGE_FAULT-ReadOnly-Address:"
#define P005 "PAGE_FAULT-UserMode-Address:"
#define P006 "PAGE_FAULT-Reserved-Address:"
#define P007 "PAGE_FAULT-DecodeAddress-Address:"
#define P008 "KERNEL_THREAD_KMALLOC_ERROR"
#define P009 "MUST_INIT_SCHED"
#define P010 "CANNOT_ALLOC_KERNEL_HEAP-KERNEL_HEAP_END:"

/* 初始化 Debug 信息 */
void init_debug(void);

/* 打印当前的段存器值 */
void get_cur_status(uint16_t* ring, uint16_t* regs1, uint16_t* regs2, uint16_t* regs3, uint16_t* regs4);

/* 内核异常 */
void panic(const char *msg);

/* 打印内核堆栈跟踪 */
void get_stack_trace(uint32_t *eips, const char **syname);

/* 强制阻塞 */
void spin(const char *name);

/* 断言失败 */
void assertion_failure(const char *exp, const char *file, int line);

#endif // INCLUDE_DEBUG_H_
