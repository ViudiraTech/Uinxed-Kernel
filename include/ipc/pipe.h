/*
 *
 *      pipe.h
 *      Pipe / FIFO subsystem interface
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IPC_PIPE_H_
#define INCLUDE_IPC_PIPE_H_

#include <libs/std/stdint.h>

void pipe_init(void);

int64_t sys_pipe(int pipefd[2]);
int64_t sys_pipe2(int pipefd[2], int flags);

#endif // INCLUDE_IPC_PIPE_H_
