#ifndef INCLUDE_NET_TCP_H_
#define INCLUDE_NET_TCP_H_

#include <net/ipv4.h>
#include <proc/task.h>

#define TCP_ENDPOINT_MAX    128U
#define TCP_ACCEPT_MAX      16U
#define TCP_RX_BUFFER_MAX   65535U
#define TCP_TX_SEGMENT_MAX  32U

typedef enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

#define TCP_STATE_CLOSED       TCP_CLOSED
#define TCP_STATE_LISTEN       TCP_LISTEN
#define TCP_STATE_SYN_SENT     TCP_SYN_SENT
#define TCP_STATE_SYN_RECEIVED TCP_SYN_RECEIVED
#define TCP_STATE_ESTABLISHED  TCP_ESTABLISHED
#define TCP_STATE_FIN_WAIT_1   TCP_FIN_WAIT_1
#define TCP_STATE_FIN_WAIT_2   TCP_FIN_WAIT_2
#define TCP_STATE_CLOSE_WAIT   TCP_CLOSE_WAIT
#define TCP_STATE_CLOSING      TCP_CLOSING
#define TCP_STATE_LAST_ACK     TCP_LAST_ACK
#define TCP_STATE_TIME_WAIT    TCP_TIME_WAIT

typedef enum tcp_event {
    TCP_EVENT_ACTIVE_OPEN,
    TCP_EVENT_RX_SYN,
    TCP_EVENT_RX_SYN_ACK,
    TCP_EVENT_RX_ACK,
    TCP_EVENT_CLOSE,
    TCP_EVENT_RX_FIN,
    TCP_EVENT_TIMEOUT,
    TCP_EVENT_RX_RST,
} tcp_event_t;

typedef struct net_tcp_segment {
        uint16_t       source_port;
        uint16_t       destination_port;
        uint32_t       sequence;
        uint32_t       acknowledgment;
        uint8_t        header_len;
        uint8_t        flags;
        const uint8_t *payload;
        size_t         payload_len;
} net_tcp_segment_t;

typedef struct tcp_endpoint tcp_endpoint_t;

tcp_endpoint_t *tcp_open(void);
void tcp_close(tcp_endpoint_t *endpoint);
int tcp_bind(tcp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int tcp_listen(tcp_endpoint_t *endpoint, unsigned backlog);
tcp_endpoint_t *tcp_accept(tcp_endpoint_t *endpoint);
int tcp_connect(tcp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int tcp_send(tcp_endpoint_t *endpoint, const void *data, size_t length);
int tcp_receive(tcp_endpoint_t *endpoint, void *data, size_t capacity);
int tcp_shutdown(tcp_endpoint_t *endpoint);
tcp_state_t tcp_get_state(const tcp_endpoint_t *endpoint);
int tcp_get_error(tcp_endpoint_t *endpoint);
int tcp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
void tcp_timer(uint64_t now_ticks);
int net_tcp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_tcp_segment_t *segment);
int net_tcp_seq_before(uint32_t a, uint32_t b);
int net_tcp_seq_after(uint32_t a, uint32_t b);
tcp_state_t net_tcp_state_next(tcp_state_t state, tcp_event_t event);

#endif
