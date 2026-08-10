/*
 *
 *      ipv6.h
 *      IPv6 protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IPV6_H_
#define INCLUDE_IPV6_H_

#include <libs/std/stddef.h>
#include <net/core/netdev.h>

#define IPV6_ADDRESS_LEN      16U
#define IPV6_HEADER_LEN       40U
#define IPV6_MIN_MTU          1280U
#define IPV6_REASSEMBLY_SLOTS 4U

#define IPV6_NEXT_HOP_BY_HOP 0U
#define IPV6_NEXT_TCP        6U
#define IPV6_NEXT_UDP        17U
#define IPV6_NEXT_ROUTING    43U
#define IPV6_NEXT_FRAGMENT   44U
#define IPV6_NEXT_ESP        50U
#define IPV6_NEXT_AH         51U
#define IPV6_NEXT_ICMP       58U
#define IPV6_NEXT_NONE       59U
#define IPV6_NEXT_DEST_OPTS  60U

typedef struct ipv6_address {
        uint8_t bytes[IPV6_ADDRESS_LEN];
} ipv6_address_t;

typedef struct ipv6_info {
        ipv6_address_t source;
        ipv6_address_t destination;
        uint32_t       flow_label;
        uint16_t       payload_length;
        uint8_t        protocol;
        uint8_t        hop_limit;
        uint32_t       fragment_id;
        uint16_t       fragment_offset;
        uint8_t        more_fragments;
} ipv6_info_t;

typedef struct net_ipv6_packet {
        ipv6_address_t source;
        ipv6_address_t destination;
        uint32_t       flow_label;
        uint32_t       fragment_id;
        uint16_t       total_len;
        uint16_t       payload_len;
        uint16_t       transport_offset;
        uint16_t       fragment_offset;
        uint8_t        protocol;
        uint8_t        hop_limit;
        uint8_t        more_fragments;
        uint8_t        has_fragment;
        const uint8_t *payload;
} net_ipv6_packet_t;

/* The input callback consumes packet on every return path. */
typedef int (*ipv6_transport_input_t)(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet);
typedef void (*ipv6_error_hook_t)(uint8_t protocol, const ipv6_address_t *source, const ipv6_address_t *destination, const void *transport,
                                  size_t transport_length, int error, uint32_t mtu);

int      net_ipv6_parse(const void *data, size_t length, net_ipv6_packet_t *packet);
uint16_t net_checksum_ipv6_pseudo(const ipv6_address_t *source, const ipv6_address_t *destination, uint8_t protocol, const void *data,
                                  size_t length);

int  ipv6_address_equal(const ipv6_address_t *left, const ipv6_address_t *right);
int  ipv6_address_is_unspecified(const ipv6_address_t *address);
int  ipv6_address_is_loopback(const ipv6_address_t *address);
int  ipv6_address_is_multicast(const ipv6_address_t *address);
int  ipv6_address_is_link_local(const ipv6_address_t *address);
int  ipv6_address_is_unicast(const ipv6_address_t *address);
void ipv6_link_local_from_mac(ipv6_address_t *address, const uint8_t mac[6]);
void ipv6_solicited_node(const ipv6_address_t *address, ipv6_address_t *multicast);
void ipv6_multicast_ethernet(const ipv6_address_t *address, uint8_t mac[6]);

int  ipv6_input(net_device_t *device, net_pbuf_t *packet);
int  ipv6_output(net_device_t *device, const ipv6_address_t *source, const ipv6_address_t *destination, uint8_t protocol, uint8_t hop_limit,
                 net_pbuf_t *packet);
int  ipv6_route(const ipv6_address_t *destination, net_device_t **device, ipv6_address_t *source, ipv6_address_t *next_hop);
int  ipv6_set_transport_handler(uint8_t protocol, ipv6_transport_input_t handler);
int  ipv6_set_error_hook(ipv6_error_hook_t hook);
void ipv6_control_error(uint8_t type, uint8_t code, uint32_t mtu, const void *quoted, size_t quoted_length);
void ipv6_timer(uint64_t now_ticks);
void ipv6_device_removed(net_device_t *device);

#endif // INCLUDE_IPV6_H_
