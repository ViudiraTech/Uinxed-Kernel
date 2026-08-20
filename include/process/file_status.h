/*
 *
 *      file_status.h
 *      Open-file status flag helpers
 *
 *      2026/8/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PROCESS_FILE_STATUS_H_
#define INCLUDE_PROCESS_FILE_STATUS_H_

#include <libs/std/stdint.h>

/* Replace only caller-selected status bits, preserving access and driver flags. */
static inline uint64_t process_file_status_flags_merge(uint64_t current, uint64_t mask, uint64_t requested)
{
    return (current & ~mask) | (requested & mask);
}

#endif // INCLUDE_PROCESS_FILE_STATUS_H_
