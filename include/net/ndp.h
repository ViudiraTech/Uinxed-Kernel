/*
 *
 *      ndp.h
 *      NDP implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_NDP_H_
#define INCLUDE_NET_NDP_H_

#include <net/ipv6.h>

#define NDP_CACHE_CAPACITY    64U
#define NDP_PENDING_PER_ENTRY 8U
#define NDP_PENDING_TOTAL     128U

int  ndp_input(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet);
int  ndp_resolve(net_device_t *device, const ipv6_address_t *address, net_pbuf_t *packet);
void ndp_learn(net_device_t *device, const ipv6_address_t *address, const uint8_t mac[6], uint64_t now_ticks);
int  ndp_router_solicit(net_device_t *device);
void ndp_device_up(net_device_t *device);
void ndp_timer(uint64_t now_ticks);
void ndp_device_removed(net_device_t *device);

#endif
