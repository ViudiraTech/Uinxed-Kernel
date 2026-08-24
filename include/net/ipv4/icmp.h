/*
 *
 *      icmp.h
 *      ICMP protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_ICMP_H_
#define INCLUDE_ICMP_H_

#include <libs/std/stddef.h>
#include <net/ipv4/ipv4.h>

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
#define ICMP_SOURCE_ROUTE_FAILED  5U
#define ICMP_NET_UNKNOWN          6U
#define ICMP_HOST_UNKNOWN         7U
#define ICMP_NET_PROHIBITED       9U
#define ICMP_HOST_PROHIBITED      10U
#define ICMP_ADMIN_PROHIBITED     13U

typedef struct icmp_endpoint icmp_endpoint_t;
typedef void (*icmp_event_callback_t)(icmp_endpoint_t *endpoint, uint32_t events, void *context);

#define ICMP_READY_READ  0x01U
#define ICMP_READY_WRITE 0x02U

/* ICMP input and error generation. */
int icmp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
int icmp_error(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, const void *original, size_t original_length);
int icmp_error_mtu(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, uint16_t mtu, const void *original, size_t original_length);

/* Raw ICMP endpoint (echo request/reply). */
icmp_endpoint_t *icmp_open(void);
void             icmp_close(icmp_endpoint_t *endpoint);
int              icmp_bind(icmp_endpoint_t *endpoint, uint32_t address);
int              icmp_connect(icmp_endpoint_t *endpoint, uint32_t address);
int              icmp_disconnect(icmp_endpoint_t *endpoint);
int              icmp_send(icmp_endpoint_t *endpoint, const void *data, size_t length, uint32_t destination, uint8_t ttl);
int              icmp_receive(icmp_endpoint_t *endpoint, void *data, size_t capacity, uint32_t *source, int peek);
uint32_t         icmp_readiness(icmp_endpoint_t *endpoint);
void             icmp_set_event_callback(icmp_endpoint_t *endpoint, icmp_event_callback_t callback, void *context);

#endif // INCLUDE_ICMP_H_
