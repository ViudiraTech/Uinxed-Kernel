/*
 *
 *      icmp.h
 *      ICMP protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_ICMP_H_
#define INCLUDE_NET_ICMP_H_

#include <net/ipv4.h>

#define ICMP_DEST_UNREACHABLE 3U
#define ICMP_ECHO_REQUEST     8U
#define ICMP_ECHO_REPLY       0U
#define ICMP_TIME_EXCEEDED    11U

#define ICMP_NET_UNREACHABLE      0U
#define ICMP_HOST_UNREACHABLE     1U
#define ICMP_PROTOCOL_UNREACHABLE 2U
#define ICMP_PORT_UNREACHABLE     3U
#define ICMP_FRAGMENTATION_NEEDED 4U
#define ICMP_REASSEMBLY_TIMEOUT   1U

int icmp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
int icmp_error(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, const void *original, size_t original_length);
int icmp_error_mtu(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, uint16_t mtu, const void *original,
                   size_t original_length);

#endif
