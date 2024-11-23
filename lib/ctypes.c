#include "ctypes.h"

/* 判断是否是空白字符 */
bool isspace(int c) {
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f');
}

/* 判断是否是数字 */
bool isdigit(int c)
{
	return (c >= '0' && c <= '9');
}