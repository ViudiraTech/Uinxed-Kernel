/*
 *
 *		syscall.h
 *		系统调用头文件
 *
 *		2024/12/15 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_SYSCALL_H_
#define INCLUDE_SYSCALL_H_

#include "idt.h"

#define MAX_SYSCALLS 256

typedef uint32_t (*syscall_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

void asm_syscall_handler(pt_regs *regs);
void syscall_init(void);

#endif // INCLUDE_SYSCALL_H_
