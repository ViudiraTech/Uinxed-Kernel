/*
 *
 *		types.h
 *		基本系统数据类型头文件
 *
 *		2024/6/29 By copi143
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NULL ((void *)0)

#ifndef __cplusplus
#	define null 0
#endif

#ifndef __cplusplus
#	define bool  _Bool
#	define true  ((bool)1)
#	define false ((bool)0)
#endif

typedef __INTPTR_TYPE__  ssize_t;
typedef __UINTPTR_TYPE__ size_t;

typedef unsigned char	uchar;
typedef unsigned short	ushort;
typedef unsigned int	uint;
typedef unsigned long	ulong;

typedef __INT8_TYPE__	int8_t;
typedef __UINT8_TYPE__	uint8_t;
typedef __INT16_TYPE__	int16_t;
typedef __UINT16_TYPE__	uint16_t;
typedef __INT32_TYPE__	int32_t;
typedef __UINT32_TYPE__	uint32_t;
typedef __INT64_TYPE__	int64_t;
typedef __UINT64_TYPE__	uint64_t;
typedef float			float32_t;
typedef double			float64_t;
typedef __float128		float128_t;

typedef int8_t     i8;
typedef uint8_t    u8;
typedef int16_t    i16;
typedef uint16_t   u16;
typedef int32_t    i32;
typedef uint32_t   u32;
typedef int64_t    i64;
typedef uint64_t   u64;
typedef float32_t  f32;
typedef float64_t  f64;
typedef float128_t f128;

typedef _Complex float  cfloat;
typedef _Complex double cdouble;
typedef _Complex double complex;

typedef _Complex __INT8_TYPE__   cint8_t;
typedef _Complex __UINT8_TYPE__  cuint8_t;
typedef _Complex __INT16_TYPE__  cint16_t;
typedef _Complex __UINT16_TYPE__ cuint16_t;
typedef _Complex __INT32_TYPE__  cint32_t;
typedef _Complex __UINT32_TYPE__ cuint32_t;
typedef _Complex __INT64_TYPE__  cint64_t;
typedef _Complex __UINT64_TYPE__ cuint64_t;
typedef _Complex float           cfloat32_t;
typedef _Complex double          cfloat64_t;

// Rainy101112: 咳咳我在这里提一下 记得把FPU做了 不然这么多浮点数定义也是白写

typedef i8 sbyte;
typedef u8 byte;

typedef const char *cstr;

#ifdef __cplusplus
}
#endif
