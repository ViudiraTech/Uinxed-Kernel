/*
 *
 *		delay.c
 *		内核延迟
 *
 *		2024/6/30 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

/* 秒级延迟函数 */
void delay_s(int seconds)
{
	int start = 0;
	for (int i = 0; i < seconds; ++i) {
		start = (int)get_sec_hex();
		while ((int)get_sec_hex() == start) {}
	}
}
