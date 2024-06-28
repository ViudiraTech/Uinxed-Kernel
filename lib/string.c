// string.s -- 提供基础内存操作和字符串处理的C语言内联函数库（基于 GPL-3.0 开源协议）
// Copyright © 2020 ViudiraTech，保留所有权利。

#include "string.h"
#include "types.h"

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
