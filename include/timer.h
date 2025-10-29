/*
 *
 *      timer.h
 *      Timer header file
 *
 *      2025/2/17 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_TIMER_H_
#define INCLUDE_TIMER_H_

#include "stdint.h"

/* Nanosecond-based delay function */
void nsleep(uint64_t ns);

/* Millisecond-based delay functions */
void usleep(uint64_t us);

/* Millisecond-based delay functions */
void msleep(uint64_t ms);

#endif // INCLUDE_TIMER_H_
