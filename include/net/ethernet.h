/*
 *
 *      ethernet.h
 *      Ethernet protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NET_ETHERNET_H_
#define INCLUDE_NET_ETHERNET_H_

#include <net/netdev.h>

#define ETH_ADDRESS_LEN 6U
#define ETH_HEADER_LEN  14U
#define ETH_TYPE_IPV4   0x0800U
#define ETH_TYPE_ARP    0x0806U
#define ETH_TYPE_IPV6   0x86ddU

extern const uint8_t ethernet_broadcast_address[ETH_ADDRESS_LEN];

typedef struct net_ethernet_frame {
        uint16_t       ether_type;
        const uint8_t *payload;
        size_t         payload_len;
} net_ethernet_frame_t;

int net_ethernet_parse(const void *data, size_t length, net_ethernet_frame_t *frame);
int ethernet_input(net_device_t *device, net_pbuf_t *packet);
int ethernet_output(net_device_t *device, net_pbuf_t *packet, const uint8_t destination[6], uint16_t type);

#endif
