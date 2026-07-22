/*
 *
 *      uaccess.h
 *      User memory access helpers
 *
 *      2026/7/20 By Rainy101112
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_UACCESS_H_
#define INCLUDE_UACCESS_H_

#include <libs/std/stddef.h>

int user_access_ok(const void *uaddr, size_t size, int write);
int copy_from_user(void *dst, const void *src, size_t size);
int copy_to_user(void *dst, const void *src, size_t size);
int strncpy_from_user(char *dst, const char *src, size_t max_size);

#endif // INCLUDE_UACCESS_H_
