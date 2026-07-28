#ifndef INCLUDE_NET_UDP_H_
#define INCLUDE_NET_UDP_H_

#include <net/ipv4.h>
#include <proc/task.h>

#define UDP_ENDPOINT_MAX  128U
#define UDP_RX_QUEUE_MAX  64U

typedef struct udp_endpoint udp_endpoint_t;

typedef struct udp_datagram {
        uint32_t source_address;
        uint16_t source_port;
        size_t   length;
} udp_datagram_t;

typedef struct net_udp_datagram {
        uint16_t       source_port;
        uint16_t       destination_port;
        const uint8_t *payload;
        size_t         payload_len;
        int            checksum_present;
} net_udp_datagram_t;

udp_endpoint_t *udp_open(void);
void udp_close(udp_endpoint_t *endpoint);
int udp_bind(udp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int udp_connect(udp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int udp_send(udp_endpoint_t *endpoint, const void *data, size_t length, uint32_t destination, uint16_t port);
int udp_receive(udp_endpoint_t *endpoint, void *data, size_t capacity, udp_datagram_t *info, int peek);
int udp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
int net_udp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_udp_datagram_t *datagram);
uint16_t udp_local_port(const udp_endpoint_t *endpoint);

#endif
