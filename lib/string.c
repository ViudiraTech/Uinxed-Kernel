// string.s -- 基础内存操作和字符串处理的内联函数库（基于 GPL-3.0 开源协议）
// Copyright © 2020 ViudiraTech，保留所有权利。
// 源于 小严awa 撰写于 2024-6-27.

#include "string.h"
#include "types.h"

int memcmp(const void* buffer1,const void* buffer2,size_t  count)
{
	if(!count) {
		return 0;
	}

	/* 当比较位数不为0时，且每位数据相等时，移动指针 */
	while(count-- && *(char*)buffer1 == *(char*)buffer2) {
		buffer1 = (char*)buffer1 + 1; // 转换类型，移动指针
		buffer2 = (char*)buffer2 + 1;
	}

	/* 返回超过比较位数之后 比较的大小 */
	return( *((uint8_t *)buffer1) - *((uint8_t *)buffer2) );    
}

inline void memcpy(uint8_t *dest, const uint8_t *src, uint32_t len)
{
	uint8_t *sr = src;
	uint8_t *dst = dest;

	while (len != 0) {
		*dst++ = *sr++;
		len--;
	}
}

inline void memset(void *dest, uint8_t val, uint32_t len)
{
	for (uint8_t *dst = (uint8_t *)dest; len != 0; len--) {
		*dst++ = val;
	}
}

inline void bzero(void *dest, uint32_t len)
{
	memset(dest, 0, len);
}

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

inline char *strcpy(char *dest, const char *src)
{
	char *tmp = dest;

	while (*src) {
		*dest++ = *src++;
	}

	*dest = '\0';	
	return tmp;
}

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

inline char *strcat(char *dest, const char *src)
{
	char *cp = dest;

	while (*cp) {
		cp++;
	}

	while ((*cp++ = *src++));
	return dest;
}

inline int strlen(const char *src)
{
	const char *eos = src;

	while (*eos++);
	return (eos - src - 1);
}
