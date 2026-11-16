/*
 *
 *      kbench.h
 *      In-kernel micro-benchmarks
 *
 *      2026/10/21 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KBENCH_H_
#define INCLUDE_KBENCH_H_

/* Register the benchmark worker when the command line carries kbench[=rounds]. */
void kbench_init(void);

#endif // INCLUDE_KBENCH_H_
