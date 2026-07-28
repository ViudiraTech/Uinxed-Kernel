#ifndef INCLUDE_NET_UDP_H_
#define INCLUDE_NET_UDP_H_

#include <net/ipv4.h>
#include <proc/task.h>

#define UDP_ENDPOINT_MAX  128U
#define UDP_RX_QUEUE_MAX  64U
#define UDP_RX_BYTES_MAX  131072U

#define UDP_READY_READ  0x01U
#define UDP_READY_WRITE 0x02U
#define UDP_READY_ERROR 0x04U

typedef struct udp_endpoint udp_endpoint_t;
typedef void (*udp_event_callback_t)(udp_endpoint_t *endpoint, uint32_t events, void *context);

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

typedef struct udp_endpoint_info {
        uint32_t local_address;
        uint32_t remote_address;
        uint16_t local_port;
        uint16_t remote_port;
        uint16_t queued_datagrams;
        uint32_t queued_bytes;
        int      connected;
} udp_endpoint_info_t;

udp_endpoint_t *udp_open(void);
void udp_close(udp_endpoint_t *endpoint);
int udp_bind(udp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int udp_connect(udp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int udp_disconnect(udp_endpoint_t *endpoint);
int udp_send(udp_endpoint_t *endpoint, const void *data, size_t length, uint32_t destination, uint16_t port);
int udp_receive(udp_endpoint_t *endpoint, void *data, size_t capacity, udp_datagram_t *info, int peek);
int udp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
int net_udp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_udp_datagram_t *datagram);
uint16_t udp_local_port(const udp_endpoint_t *endpoint);
uint32_t udp_readiness(udp_endpoint_t *endpoint);
int udp_get_info(udp_endpoint_t *endpoint, udp_endpoint_info_t *info);
void udp_set_event_callback(udp_endpoint_t *endpoint, udp_event_callback_t callback, void *context);
wait_queue_t *udp_wait_queue(udp_endpoint_t *endpoint);

#endif
