/*
 *
 *		rand.c
 *		随机数相关库
 *
 *		2025/1/8 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#include "rand.h"

/* 随机数种子 */
static unsigned int rand_seed = 0;

/* 设置随机数种子 */
void srand(unsigned int seed)
{
	rand_seed = seed;
}

/* 随机数生成 */
int rand(void)
{
	rand_seed ^= rand_seed << 13;
	rand_seed ^= rand_seed >> 17;
	rand_seed ^= rand_seed << 5;
	return rand_seed & 2147483647;
}
