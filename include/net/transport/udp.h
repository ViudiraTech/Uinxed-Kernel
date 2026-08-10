/*
 *
 *      udp.h
 *      UDP protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_UDP_H_
#define INCLUDE_UDP_H_

#include <libs/std/stddef.h>
#include <net/abi/inet.h>
#include <net/ipv4/ipv4.h>
#include <net/ipv6/ipv6.h>
#include <process/task.h>

#define UDP_ENDPOINT_MAX 128U
#define UDP_RX_QUEUE_MAX 64U
#define UDP_RX_BYTES_MAX 131072U

#define UDP_READY_READ  0x01U
#define UDP_READY_WRITE 0x02U
#define UDP_READY_ERROR 0x04U

typedef struct udp_endpoint udp_endpoint_t;
typedef void (*udp_event_callback_t)(udp_endpoint_t *endpoint, uint32_t events, void *context);

typedef struct udp_datagram {
        uint16_t       family;
        uint32_t       source_address;
        ipv6_address_t source_address6;
        uint16_t       source_port;
        size_t         length;
} udp_datagram_t;

typedef struct net_udp_datagram {
        uint16_t       source_port;
        uint16_t       destination_port;
        const uint8_t *payload;
        size_t         payload_len;
        int            checksum_present;
} net_udp_datagram_t;

typedef struct udp_endpoint_info {
        uint16_t       family;
        uint32_t       local_address;
        uint32_t       remote_address;
        ipv6_address_t local_address6;
        ipv6_address_t remote_address6;
        uint16_t       local_port;
        uint16_t       remote_port;
        uint16_t       queued_datagrams;
        uint32_t       queued_bytes;
        int            connected;
} udp_endpoint_info_t;

udp_endpoint_t *udp_open(void);
udp_endpoint_t *udp_open_family(uint16_t family);
void            udp_close(udp_endpoint_t *endpoint);
int             udp_bind(udp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int             udp_bind6(udp_endpoint_t *endpoint, const ipv6_address_t *address, uint16_t port);
int             udp_connect(udp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int             udp_connect6(udp_endpoint_t *endpoint, const ipv6_address_t *address, uint16_t port);
int             udp_disconnect(udp_endpoint_t *endpoint);
int             udp_send(udp_endpoint_t *endpoint, const void *data, size_t length, uint32_t destination, uint16_t port);
int udp_send6(udp_endpoint_t *endpoint, const void *data, size_t length, const ipv6_address_t *destination, uint16_t port, uint8_t hop_limit);
int udp_receive(udp_endpoint_t *endpoint, void *data, size_t capacity, udp_datagram_t *info, int peek);
int udp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
int udp_input6(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet);
int net_udp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_udp_datagram_t *datagram);
int net_udp_parse6(const void *data, size_t length, const struct in6_addr *source, const struct in6_addr *destination,
                   net_udp_datagram_t *datagram);
uint16_t      udp_local_port(const udp_endpoint_t *endpoint);
uint32_t      udp_readiness(udp_endpoint_t *endpoint);
int           udp_get_info(udp_endpoint_t *endpoint, udp_endpoint_info_t *info);
void          udp_set_v6only(udp_endpoint_t *endpoint, int enabled);
void          udp_set_event_callback(udp_endpoint_t *endpoint, udp_event_callback_t callback, void *context);
wait_queue_t *udp_wait_queue(udp_endpoint_t *endpoint);

#endif // INCLUDE_UDP_H_
