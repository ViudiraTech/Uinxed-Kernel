/*
 *
 *      uaccess.h
 *      User memory access helpers
 *
 *      2026/7/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_UACCESS_H_
#define INCLUDE_UACCESS_H_

#include <libs/std/stddef.h>

struct process;

/* Range validation for user pointers. */
int user_range_ok(const void *uaddr, size_t size);
int user_access_ok(const void *uaddr, size_t size, int write);
int user_access_ok_process(struct process *proc, const void *uaddr, size_t size, int write);

/* Copy to/from user memory, faulting on bad addresses. */
int copy_from_user(void *dst, const void *src, size_t size);
int copy_to_user(void *dst, const void *src, size_t size);
int copy_from_user_process_nofault(struct process *proc, void *dst, const void *src, size_t size);
int copy_to_user_process_nofault(struct process *proc, void *dst, const void *src, size_t size);
int copy_from_user_process_nofault_current(struct process *proc, void *dst, const void *src, size_t size);
int copy_to_user_process_nofault_current(struct process *proc, void *dst, const void *src, size_t size);
int clear_user_process(struct process *proc, void *dst, size_t size);

/* Bounded string reads from user memory. */
int strnlen_user(const char *src, size_t max_size);
int strncpy_from_user(char *dst, const char *src, size_t max_size);

#endif // INCLUDE_UACCESS_H_
