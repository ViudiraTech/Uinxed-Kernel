/*
 *
 *      ipv4.h
 *      IPv4 protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IPV4_H_
#define INCLUDE_IPV4_H_

#include <libs/std/stddef.h>
#include <net/core/netdev.h>

#define IPV4_HEADER_MIN 20U
#define IPV4_PROTO_ICMP 1U
#define IPV4_PROTO_TCP  6U
#define IPV4_PROTO_UDP  17U

#define IPV4_FLAG_DF          0x4000U
#define IPV4_FLAG_MF          0x2000U
#define IPV4_FRAGMENT_MASK    0x1fffU
#define IPV4_REASSEMBLY_SLOTS 4U

typedef void (*ipv4_error_hook_t)(uint8_t protocol, uint32_t source, uint32_t destination, const void *transport, size_t transport_length,
                                  int error, uint32_t mtu);

typedef struct ipv4_info {
        uint32_t source;
        uint32_t destination;
        uint16_t payload_length;
        uint8_t  protocol;
        uint8_t  ttl;
        uint16_t identification;
        uint16_t fragment_offset;
        uint8_t  more_fragments;
} ipv4_info_t;

typedef struct net_ipv4_packet {
        uint8_t        header_len;
        uint16_t       total_len;
        uint8_t        protocol;
        uint16_t       identification;
        uint16_t       fragment_offset;
        uint8_t        more_fragments;
        uint32_t       source;
        uint32_t       destination;
        const uint8_t *payload;
        size_t         payload_len;
} net_ipv4_packet_t;

int  net_ipv4_parse(const void *data, size_t length, net_ipv4_packet_t *packet);
int  ipv4_input(net_device_t *device, net_pbuf_t *packet);
int  ipv4_output(net_device_t *device, uint32_t source, uint32_t destination, uint8_t protocol, uint8_t ttl, net_pbuf_t *packet);
int  ipv4_route(uint32_t destination, net_device_t **device, uint32_t *next_hop);
int  ipv4_set_error_hook(ipv4_error_hook_t hook);
void ipv4_control_error(uint8_t type, uint8_t code, uint32_t mtu, const void *quoted, size_t quoted_length);
void ipv4_timer(uint64_t now_ticks);
void ipv4_device_removed(net_device_t *device);

#endif // INCLUDE_IPV4_H_
