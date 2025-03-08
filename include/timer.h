/*
 *
 *		timer.h
 *		定时器头文件
 *
 *		2025/2/17 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_TIMER_H_
#define INCLUDE_TIMER_H_

#include "stdint.h"

/* 基于微秒的延迟函数 */
void sleep(uint64_t micro);

/* 基于纳秒的延迟函数 */
void usleep(uint64_t nano);

#endif // INCLUDE_TIMER_H_
