/*
 *
 *      numa.h
 *      NUMA memory-policy syscall interface
 *
 *      2026/10/4 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSCALL_NUMA_H_
#define INCLUDE_SYSCALL_NUMA_H_

#include <libs/std/stdint.h>

int64_t sys_set_mempolicy(uint64_t mode, uint64_t nodes, uint64_t maxnode, uint64_t unused3, uint64_t unused4, uint64_t unused5);
int64_t sys_get_mempolicy(uint64_t mode, uint64_t nodes, uint64_t maxnode, uint64_t address, uint64_t flags, uint64_t unused5);
int64_t sys_mbind(uint64_t address, uint64_t length, uint64_t mode, uint64_t nodes, uint64_t maxnode, uint64_t flags);

#endif // INCLUDE_SYSCALL_NUMA_H_
