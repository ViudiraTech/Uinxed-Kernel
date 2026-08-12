/*
 *
 *      packet.h
 *      Network packet API
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PACKET_H_
#define INCLUDE_PACKET_H_

#include <libs/std/stddef.h>
#include <net/core/pbuf.h>

typedef net_pbuf_t net_packet_t;
typedef void (*net_packet_release_t)(void *context, void *data);

/* Wrap external data in a packet, or access an existing one. */
int    net_packet_init_external(net_packet_t *packet, void *data, size_t length, net_packet_release_t release, void *context);
void   net_packet_get(net_packet_t *packet);
void   net_packet_put(net_packet_t *packet);
void  *net_packet_data(net_packet_t *packet);
size_t net_packet_length(const net_packet_t *packet);

#endif // INCLUDE_PACKET_H_
