#ifndef INCLUDE_NET_IPV4_H_
#define INCLUDE_NET_IPV4_H_

#include <net/netdev.h>

#define IPV4_HEADER_MIN 20U
#define IPV4_PROTO_ICMP 1U
#define IPV4_PROTO_TCP  6U
#define IPV4_PROTO_UDP  17U

typedef struct ipv4_info {
        uint32_t source;
        uint32_t destination;
        uint16_t payload_length;
        uint8_t  protocol;
        uint8_t  ttl;
} ipv4_info_t;

typedef struct net_ipv4_packet {
        uint8_t        header_len;
        uint16_t       total_len;
        uint8_t        protocol;
        uint32_t       source;
        uint32_t       destination;
        const uint8_t *payload;
        size_t         payload_len;
} net_ipv4_packet_t;

int net_ipv4_parse(const void *data, size_t length, net_ipv4_packet_t *packet);
int ipv4_input(net_device_t *device, net_pbuf_t *packet);
int ipv4_output(net_device_t *device, uint32_t source, uint32_t destination, uint8_t protocol, uint8_t ttl, net_pbuf_t *packet);
int ipv4_route(uint32_t destination, net_device_t **device, uint32_t *next_hop);

#endif
