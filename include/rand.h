/*
 *
 *		rand.h
 *		随机数相关库头文件
 *
 *		2025/1/8 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_RAND_H_
#define INCLUDE_RAND_H_

/* 设置随机数种子 */
void srand(unsigned int seed);

/* 随机数生成 */
int rand(void);

#endif // INCLUDE_RAND_H_
