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

#define MAX_PRECISION (10)

static const double rounders[MAX_PRECISION + 1] = {
	0.5,				// 0
	0.05,				// 1
	0.005,				// 2
	0.0005,				// 3
	0.00005,			// 4
	0.000005,			// 5
	0.0000005,			// 6
	0.00000005,			// 7
	0.000000005,		// 8
	0.0000000005,		// 9
	0.00000000005		// 10
};

/* 比较两个内存区域的前count个字节 */
int memcmp(const void* buffer1,const void* buffer2,size_t  count);

/* 将len个字节从源地址复制到目标地址 */
void memcpy(uint8_t *dest, const uint8_t *src, uint32_t len);

/* 将目标内存区域的前len个字节设置为值val */
void memset(void *dest, uint8_t val, uint32_t len);

/* 将目标内存区域的前len个字节设置为0 */
void bzero(void *dest, uint32_t len);

/* 清除一个字符数组s的内存 */
void memclean(char *s, int len);

/* 比较两个字符串 */
int strcmp(const char *str1, const char *str2);

/* 将字符串src复制到dest */
char *strcpy(char *dest, const char *src);

/* 将字符串的前len个字符复制到dest */
char *strncpy(char *dest, const char *src, uint32_t len);

/* 将字符串src连接到dest的末尾 */
char *strcat(char *dest, const char *src);

/* 查找字符串中的一个字符并返回该字符在字符串中第一次出现的位置 */
char *strchr(char *str, int c);

/* 返回字符串src的长度 */
int strlen(const char *src);

/* 删除字符串中指定位置的字符 */
void delete_char(char *str, int pos);

/* 在字符串的指定位置插入一个字符 */
void insert_char(char *str, int pos, char ch);

/* 在字符串的指定位置插入另一个字符串 */
void insert_str(char *str, char *insert_str, int pos);

/* 将字符串中的所有小写字母转换为大写字母 */
void strtoupper(char *str);

/* 从文件件路径中获取文件名 */
char *get_filename(char *path);

/* 将字符串数字转换为整数数字 */
int atoi(char* pstr);

/* 把浮点数转换成字符数组 */
char *ftoa(double f, char *buf, int precision);

#endif // INCLUDE_STRING_H_
