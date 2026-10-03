/*
 *
 *      ndp.h
 *      NDP implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NDP_H_
#define INCLUDE_NDP_H_

#include <net/ipv6/ipv6.h>

/* Pending neighbours queued per cache entry. */
#define NDP_PENDING_PER_ENTRY 8U

/* NDP packet processing, neighbor cache, and router discovery. */
#if CONFIG_INET && CONFIG_NET
void ndp_init(void);
#else
static inline void ndp_init(void) {}
#endif

/* NDP (IPv6 Neighbor Discovery) input. */
int ndp_input(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet);

/* NDP (IPv6 Neighbor Discovery) resolve. */
int ndp_resolve(net_device_t *device, const ipv6_address_t *address, net_pbuf_t *packet);

/* NDP (IPv6 Neighbor Discovery) learn. */
void ndp_learn(net_device_t *device, const ipv6_address_t *address, const uint8_t mac[6], uint64_t now_ticks);

/* NDP (IPv6 Neighbor Discovery) router solicit. */
int ndp_router_solicit(net_device_t *device);

#if CONFIG_INET && CONFIG_NET

/* NDP (IPv6 Neighbor Discovery) device up. */
void ndp_device_up(net_device_t *device);

/* NDP (IPv6 Neighbor Discovery) timer. */
void ndp_timer(uint64_t now_ticks);

/* NDP (IPv6 Neighbor Discovery) device removed. */
void ndp_device_removed(net_device_t *device);

#else
static inline void ndp_device_up(net_device_t *) {}
static inline void ndp_timer(uint64_t) {}
static inline void ndp_device_removed(net_device_t *) {}
#endif

#endif // INCLUDE_NDP_H_
