/*
 *
 *		string.s
 *		基础内存操作和字符串处理的内联函数库
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "string.h"
#include "types.h"
#include "memory.h"

/* 比较两个内存区域的前count个字节 */
inline int memcmp(const void *buffer1, const void *buffer2, size_t count)
{
    const char *a = buffer1;
    const char *b = buffer2;
    while (count-- > 0) {
        if (*a != *b) return *a > *b ? 1 : -1;
        a++, b++;
    }
    return 0;
}

/* 将len个字节从源地址复制到目标地址 */
inline void memcpy(uint8_t *dest, const uint8_t *src, uint32_t len)
{
	uint8_t *sr = (uint8_t *)src;
	uint8_t *dst = dest;

	while (len != 0) {
		*dst++ = *sr++;
		len--;
	}
}

/* 将目标内存区域的前len个字节设置为值val */
inline void memset(void *dest, uint8_t val, uint32_t len)
{
	for (uint8_t *dst = (uint8_t *)dest; len != 0; len--) {
		*dst++ = val;
	}
}

/* 将目标内存区域的前len个字节设置为0 */
inline void bzero(void *dest, uint32_t len)
{
	memset(dest, 0, len);
}

/* 清除一个字符数组s的内存 */
void memclean(char *s, int len)
{
	int i;
	for (i = 0; i != len; i++) {
		s[i] = 0;
	}
	return;
}

/* 比较两个字符串 */
inline int strcmp(const char *dest, const char *src)
{
	int ret = 0 ;

	while(!(ret = *(unsigned char *)src - *(unsigned char *)dest) && *dest) {
		++src;
		++dest;
	}
	if (ret < 0) {
		ret = -1;
	}
	else if (ret > 0) {
		ret = 1;
	}
	return ret;
}

/* 将字符串src复制到dest */
inline char *strcpy(char *dest, const char *src)
{
	char *tmp = dest;

	while (*src) {
		*dest++ = *src++;
	}
	*dest = '\0';	
	return tmp;
}

/* 将字符串的前len个字符复制到dest */
char *strncpy(char *dest, const char *src, uint32_t len)
{
	char *dst = dest;

	while (len > 0) {
		while (*src) {
			*dest++ = *src++;
		}
		len--;
	}
	*dest = '\0';
	return dst;
}

/* 将字符串src连接到dest的末尾 */
inline char *strcat(char *dest, const char *src)
{
	char *cp = dest;

	while (*cp) {
		cp++;
	}
	while ((*cp++ = *src++));
	return dest;
}

/* 查找字符串中的一个字符并返回该字符在字符串中第一次出现的位置 */
char *strchr(char *str, int c)
{
	for (; *str != 0; ++str) {
		if (*str == c) {
			return str;
		}
	}
	return 0;
}

/* 查找字符串中的一个字符并返回该字符在字符串中第一次出现的位置，找不到返回空指针 */
char *strchrnul(const char* s, int c)
{
	char* p = (char*)s;
	while (*p && *p != (char)c) {
		p++;
	}
	return p;
}

/* 返回字符串src的长度 */
inline int strlen(const char *src)
{
	const char *eos = src;

	while (*eos++);
	return (eos - src - 1);
}

/* 删除字符串中指定位置的字符 */
void delete_char(char *str, int pos)
{
	int i;
	for (i = pos; i < strlen(str); i++) {
		str[i] = str[i + 1];
	}
}

/* 在字符串的指定位置插入一个字符 */
void insert_char(char *str, int pos, char ch)
{
	int i;
	for (i = strlen(str); i >= pos; i--) {
		str[i + 1] = str[i];
	}
	str[pos] = ch;
}

/* 在字符串的指定位置插入另一个字符串 */
void insert_str(char *str, char *insert_str, int pos)
{
	for (int i = 0; i < strlen(insert_str); i++) {
		insert_char(str, pos + i, insert_str[i]);
	}
}

/* 将字符串中的所有字母转换为大写 */
char *strupr(char *src)
{
	while (*src != '\0')
	{
		if (*src >= 'a' && *src <= 'z')
			*src -= 32;
		src++;
	}
	return src;
}

/* 将字符串中的所有字母转换为小写 */
char *strlwr(char *src)
{
	while (*src != '\0')
	{
		if (*src > 'A' && *src <= 'Z'){
			//*src += 0x20; 
			*src += 32;
		}
		src++;
	}
	return src;
}

/* 拷贝字符串副本并返回 */
void *strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	void *p = (void *)kmalloc(len);
	if (p != 0) memcpy(p, (uint8_t *)s, len);
	return p;
}

/* 比较指定长度两个字符串的大小 */
int strncmp(const char *s1, const char *s2, size_t n)
{
	const unsigned char *p1 = (const unsigned char *)s1,
	*p2 = (const unsigned char *)s2;
	while (n-- > 0) {
		if (*p1 != *p2) return *p1 - *p2;
		if (*p1 == '\0') return 0;
		p1++, p2++;
	}
	return 0;
}

/* 字符串相等判断 */
int streq(const char *s1, const char *s2)
{
	return strcmp(s1, s2) == 0;
}
