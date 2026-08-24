/*
 *
 *      tcp.h
 *      TCP protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TCP_H_
#define INCLUDE_TCP_H_

#include <kernel/timer/timer.h>
#include <libs/std/stddef.h>
#include <net/abi/inet.h>
#include <net/ipv4/ipv4.h>
#include <net/ipv6/ipv6.h>
#include <process/task.h>

#define TCP_ENDPOINT_MAX    128U
#define TCP_ACCEPT_MAX      16U
#define TCP_RX_BUFFER_MAX   65535U
#define TCP_TX_SEGMENT_MAX  32U
#define TCP_OOO_SEGMENT_MAX 16U

#define TCP_KEEPIDLE_DEFAULT_TICKS  ((uint64_t)7200U * TIMER_HZ)
#define TCP_KEEPINTVL_DEFAULT_TICKS ((uint64_t)75U * TIMER_HZ)
#define TCP_KEEPCNT_DEFAULT         9U
#define TCP_SYN_RETRIES_DEFAULT     6U
#define TCP_DATA_RETRIES_DEFAULT    15U

#define TCP_READY_READ   0x01U
#define TCP_READY_WRITE  0x02U
#define TCP_READY_ERROR  0x04U
#define TCP_READY_HANGUP 0x08U
#define TCP_READY_ACCEPT 0x10U

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
typedef void (*tcp_event_callback_t)(tcp_endpoint_t *endpoint, uint32_t events, void *context);

typedef enum tcp_option {
    TCP_OPTION_KEEPALIVE,
    TCP_OPTION_KEEPIDLE_TICKS,
    TCP_OPTION_KEEPINTVL_TICKS,
    TCP_OPTION_KEEPCNT,
    TCP_OPTION_SYN_RETRIES,
    TCP_OPTION_DATA_RETRIES,
} tcp_option_t;

typedef struct tcp_endpoint_info {
        uint16_t       family;
        uint32_t       local_address;
        uint32_t       remote_address;
        ipv6_address_t local_address6;
        ipv6_address_t remote_address6;
        uint16_t       local_port;
        uint16_t       remote_port;
        tcp_state_t    state;
        uint32_t       receive_queued;
        uint32_t       send_unacknowledged;
        uint32_t       congestion_window;
        uint16_t       receive_window;
        uint16_t       send_window;
        uint16_t       peer_mss;
        uint32_t       retransmit_timeout;
        uint32_t       retransmissions;
        uint32_t       keepalive_probes;
        uint32_t       persist_probes;
        uint32_t       duplicate_acks;
        uint32_t       queued_segments;
        uint64_t       last_received_ticks;
        uint64_t       next_timer_ticks;
        uint8_t        keepalive_enabled;
} tcp_endpoint_info_t;

/* TCP endpoint lifecycle and socket-like operations. */
tcp_endpoint_t *tcp_open(void);
tcp_endpoint_t *tcp_open_family(uint16_t family);
void            tcp_close(tcp_endpoint_t *endpoint);
int             tcp_bind(tcp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int             tcp_bind6(tcp_endpoint_t *endpoint, const ipv6_address_t *address, uint16_t port);
int             tcp_listen(tcp_endpoint_t *endpoint, unsigned backlog);
tcp_endpoint_t *tcp_accept(tcp_endpoint_t *endpoint);
int             tcp_connect(tcp_endpoint_t *endpoint, uint32_t address, uint16_t port);
int             tcp_connect6(tcp_endpoint_t *endpoint, const ipv6_address_t *address, uint16_t port);
int             tcp_send(tcp_endpoint_t *endpoint, const void *data, size_t length);
int             tcp_receive(tcp_endpoint_t *endpoint, void *data, size_t capacity);
int             tcp_shutdown(tcp_endpoint_t *endpoint);

/* Endpoint state, options, and readiness. */
tcp_state_t   tcp_get_state(const tcp_endpoint_t *endpoint);
int           tcp_get_error(tcp_endpoint_t *endpoint);
int           tcp_set_option(tcp_endpoint_t *endpoint, tcp_option_t option, uint32_t value);
int           tcp_get_option(tcp_endpoint_t *endpoint, tcp_option_t option, uint32_t *value);
void          tcp_set_v6only(tcp_endpoint_t *endpoint, int enabled);
uint32_t      tcp_readiness(tcp_endpoint_t *endpoint);
int           tcp_get_info(tcp_endpoint_t *endpoint, tcp_endpoint_info_t *info);
void          tcp_set_event_callback(tcp_endpoint_t *endpoint, tcp_event_callback_t callback, void *context);
wait_queue_t *tcp_wait_queue(tcp_endpoint_t *endpoint);

/* Protocol entry points and packet parsing. */
int         tcp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet);
int         tcp_input6(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet);
void        tcp_control_error(uint32_t source, uint32_t destination, const void *quoted, size_t quoted_length, int error, uint32_t mtu);
void        tcp_timer(uint64_t now_ticks);
int         net_tcp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_tcp_segment_t *segment);
int         net_tcp_parse6(const void *data, size_t length, const struct in6_addr *source, const struct in6_addr *destination, net_tcp_segment_t *segment);
int         net_tcp_seq_before(uint32_t a, uint32_t b);
int         net_tcp_seq_after(uint32_t a, uint32_t b);
tcp_state_t net_tcp_state_next(tcp_state_t state, tcp_event_t event);

#endif // INCLUDE_TCP_H_
