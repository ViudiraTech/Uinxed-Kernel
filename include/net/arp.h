/*
 *
 *      arp.h
 *      ARP protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_ARP_H_
#define INCLUDE_NET_ARP_H_

#include <net/netdev.h>

#define ARP_CACHE_CAPACITY    64U
#define ARP_PENDING_PER_ENTRY 64U
#define ARP_PENDING_TOTAL     256U

typedef struct net_arp_packet {
        uint16_t hardware_type;
        uint16_t protocol_type;
        uint8_t  hardware_len;
        uint8_t  protocol_len;
        uint16_t operation;
} net_arp_packet_t;

int  net_arp_parse(const void *data, size_t length, net_arp_packet_t *arp);
int  arp_input(net_device_t *device, net_pbuf_t *packet);
int  arp_resolve(net_device_t *device, uint32_t ipv4, net_pbuf_t *packet);
int  arp_request(net_device_t *device, uint32_t ipv4);
void arp_learn(net_device_t *device, uint32_t ipv4, const uint8_t address[6], uint64_t now_ticks);
void arp_timer(uint64_t now_ticks);
void arp_device_removed(net_device_t *device);

#endif
