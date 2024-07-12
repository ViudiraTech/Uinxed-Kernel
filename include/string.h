/*
 *
 *		string.h
 *		基础内存操作和字符串处理的内联函数库头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_STRING_H_
#define INCLUDE_STRING_H_

#include "types.h"

/* 比较两个内存区域的前count个字节 */
int memcmp(const void* buffer1,const void* buffer2,size_t  count);

/* 将len个字节从源地址复制到目标地址 */
void memcpy(uint8_t *dest, const uint8_t *src, uint32_t len);

/* 将目标内存区域的前len个字节设置为值val */
void memset(void *dest, uint8_t val, uint32_t len);

/* 将目标内存区域的前len个字节设置为0 */
void bzero(void *dest, uint32_t len);

/* 比较两个字符串 */
int strcmp(const char *str1, const char *str2);

/* 将字符串src复制到dest */
char *strcpy(char *dest, const char *src);

/* 将字符串的前len个字符复制到dest */
char *strncpy(char *dest, const char *src, uint32_t len);

/* 将字符串src连接到dest的末尾 */
char *strcat(char *dest, const char *src);

/* 返回字符串src的长度 */
int strlen(const char *src);

#endif // INCLUDE_STRING_H_
