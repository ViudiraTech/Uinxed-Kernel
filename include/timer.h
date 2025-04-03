/*
 *
 *		timer.h
 *		Timer header file
 *
 *		2025/2/17 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_TIMER_H_
#define INCLUDE_TIMER_H_

#include "stdint.h"

/* Microsecond-based delay functions */
void sleep(uint64_t micro);

/* Nanosecond-based delay function */
void usleep(uint64_t nano);

#endif // INCLUDE_TIMER_H_
