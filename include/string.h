/*
 *
 *		string.h
 *		基础内存操作和字符串处理的内联函数库头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
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

/* 将字符串中的所有字母转换为大写 */
char *strupr(char *src);

/* 将字符串中的所有字母转换为小写 */
char *strlwr(char *src);

/* 拷贝字符串副本并返回 */
void *strdup(const char *s);

/* 比较指定长度两个字符串的大小 */
int strncmp(const char *s1, const char *s2, size_t n);

#endif // INCLUDE_STRING_H_
