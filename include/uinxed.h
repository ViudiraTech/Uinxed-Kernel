/*
 *
 *		uinxed.h
 *		内核描述头文件
 *
 *		2024/7/23 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_UINXED_H_
#define INCLUDE_UINXED_H_

#define BUILD_DATE __DATE__			// 编译日期
#define BUILD_TIME __TIME__			// 编译时间
#define KERNL_VERS "0.0.250308"		// 版本格式: v[主版本].[补丁版本].[年月日]

/* 编译器判断 */
#if defined(__clang__)
#	define COMPILER_NAME "clang"
#	define STRINGIFY(x) #x
#	define EXPAND(x) STRINGIFY(x)
#	define COMPILER_VERSION EXPAND(__clang_major__.__clang_minor__.__clang_patchlevel__)
#elif defined(__GNUC__)
#	define COMPILER_NAME "gcc"
#	define STRINGIFY(x) #x
#	define EXPAND(x) STRINGIFY(x)
#	define COMPILER_VERSION EXPAND(__GNUC__.__GNUC_MINOR__.__GNUC_PATCHLEVEL__)
#else
#	error "Unknown compiler"
#endif

#endif // INCLUDE_UINXED_H_
