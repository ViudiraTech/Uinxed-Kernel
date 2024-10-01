/*
 *
 *		math.s
 *		数学处理的内联函数库
 *
 *		2024/10/2 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "math.h"

/* sin运算 */
double sin(double x)
{
	asm volatile("fldl %0 \n"
                 "fsin \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/* cos运算 */
double cos(double x)
{
	asm volatile("fldl %0 \n"
                 "fcos \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/* tan运算 */
double tan(double x)
{
	asm volatile("fldl %0 \n"
                 "fptan \n"
                 "fstpl %0\n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/* sqrt运算 */
double sqrt(double x)
{
	asm volatile("fldl %0 \n"
                 "fsqrt \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}

/* log2运算 */
double log2(double x)
{
	asm volatile("fld1 \n"
                 "fldl %0 \n"
                 "fyl2x \n"
                 "fwait \n"
                 "fstpl %0\n"
                 : "+m"(x));
	return x;
}
