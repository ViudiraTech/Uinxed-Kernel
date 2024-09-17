/*
 *
 *		fpu.h
 *		fpu浮点协处理器头文件
 *
 *		2024/9/17 By _min0911
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_FPU_H_
#define INCLUDE_FPU_H_

#include "idt.h"

/* 初始化FPU */
void init_fpu(void);

/* FPU中断 */
void fpu_handler(pt_regs *regs);

#endif // INCLUDE_FPU_H_
