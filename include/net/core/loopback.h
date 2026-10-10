/*
 *
 *      loopback.h
 *      Loopback network device header file
 *
 *      2026/8/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_LOOPBACK_H_
#define INCLUDE_LOOPBACK_H_

struct net_namespace;

/* Create and register the 'lo' interface (127.0.0.1/8, always up). */
#if CONFIG_INET && CONFIG_NET
void loopback_init(void);
int loopback_namespace_init(struct net_namespace *ns);
void loopback_namespace_destroy(struct net_namespace *ns);
#else
static inline void loopback_init(void) {}
static inline int loopback_namespace_init(struct net_namespace *ns) { (void)ns; return 0; }
static inline void loopback_namespace_destroy(struct net_namespace *ns) { (void)ns; }
#endif

#endif // INCLUDE_LOOPBACK_H_
