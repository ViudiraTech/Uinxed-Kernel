/*
 *
 *      icmpv6.h
 *      ICMPv6 protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_ICMPV6_H_
#define INCLUDE_NET_ICMPV6_H_

#include <net/ipv6.h>

#define ICMPV6_DEST_UNREACHABLE  1U
#define ICMPV6_PACKET_TOO_BIG    2U
#define ICMPV6_TIME_EXCEEDED     3U
#define ICMPV6_PARAMETER_PROBLEM 4U
#define ICMPV6_ECHO_REQUEST      128U
#define ICMPV6_ECHO_REPLY        129U
#define ICMPV6_ROUTER_SOLICIT    133U
#define ICMPV6_ROUTER_ADVERT     134U
#define ICMPV6_NEIGHBOR_SOLICIT  135U
#define ICMPV6_NEIGHBOR_ADVERT   136U

#define ICMPV6_NO_ROUTE            0U
#define ICMPV6_ADDRESS_UNREACHABLE 3U
#define ICMPV6_PORT_UNREACHABLE    4U
#define ICMPV6_HOP_LIMIT_EXCEEDED  0U
#define ICMPV6_REASSEMBLY_TIMEOUT  1U
#define ICMPV6_BAD_NEXT_HEADER     1U

int icmpv6_input(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet);
int icmpv6_error(net_device_t *device, const ipv6_address_t *destination, uint8_t type, uint8_t code, uint32_t value, const void *original,
                 size_t original_length);

#endif
