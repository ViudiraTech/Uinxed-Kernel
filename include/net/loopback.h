/*
 *
 *      loopback.h
 *      Loopback network device header file
 *
 *      2026/8/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_LOOPBACK_H_
#define INCLUDE_NET_LOOPBACK_H_

/* Create and register the 'lo' interface (127.0.0.1/8, always up). */
void loopback_init(void);

#endif // INCLUDE_NET_LOOPBACK_H_
