/*
 *
 *		parallel.h
 *		Parallel Port Header File
 *
 *		2024/9/8 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_PARALLEL_H_
#define INCLUDE_PARALLEL_H_

#define LPT1_PORT_BASE		0x378				// Parallel interface base address
#define LPT1_PORT_DATA		LPT1_PORT_BASE		// Parallel data interface
#define LPT1_PORT_STATUS	LPT1_PORT_BASE + 1	// Parallel Status Interface
#define LPT1_PORT_CONTROL	LPT1_PORT_BASE + 2	// Parallel control interface

/* Waiting for the parallel port to become ready */
void wait_parallel_ready(void);

/* Write to parallel port */
void parallel_write(unsigned char c);

#endif // INCLUDE_PARALLEL_H_
