/*
 *
 *      pipe.h
 *      Pipe / FIFO subsystem interface
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PIPE_H_
#define INCLUDE_PIPE_H_

#include <libs/std/stdint.h>

/* Initialize the pipe subsystem. */
void pipe_init(void);

/* Create a pipe, returning both ends in pipefd[2]. */
int64_t sys_pipe(int pipefd[2]);

/* Create a pipe honoring flags (e.g. O_CLOEXEC | O_NONBLOCK). */
int64_t sys_pipe2(int pipefd[2], int flags);

#endif // INCLUDE_PIPE_H_
