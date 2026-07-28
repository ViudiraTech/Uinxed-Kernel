/*
 *
 *      pbuf.h
 *      Packet buffer API
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_PBUF_H_
#define INCLUDE_NET_PBUF_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

#define NET_PBUF_HEADROOM 64U
#define NET_PBUF_MAX_SIZE 65535U

typedef struct net_pbuf {
        uint8_t *storage;
        uint8_t *data;
        size_t   length;
        size_t   capacity;
        uint32_t refs;
        void (*release)(void *context, void *data);
        void   *release_context;
        uint8_t external;
} net_pbuf_t;

net_pbuf_t *net_pbuf_alloc(size_t payload_length, size_t headroom);
net_pbuf_t *net_pbuf_from(const void *data, size_t length, size_t headroom);
net_pbuf_t *net_pbuf_clone(const net_pbuf_t *pbuf, size_t headroom);
void        net_pbuf_ref(net_pbuf_t *pbuf);
void        net_pbuf_free(net_pbuf_t *pbuf);
void       *net_pbuf_push(net_pbuf_t *pbuf, size_t length);
void       *net_pbuf_pull(net_pbuf_t *pbuf, size_t length);
int         net_pbuf_trim(net_pbuf_t *pbuf, size_t length);
size_t      net_pbuf_headroom(const net_pbuf_t *pbuf);

#endif
