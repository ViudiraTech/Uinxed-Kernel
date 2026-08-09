/*
 *
 *      tcp.c
 *      TCP protocol implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/endian.h>
#include <net/tcp.h>
#include <proc/sched.h>

#define TCP_HEADER_LEN      20U
#define TCP_FLAG_FIN        0x01U
#define TCP_FLAG_SYN        0x02U
#define TCP_FLAG_RST        0x04U
#define TCP_FLAG_PSH        0x08U
#define TCP_FLAG_ACK        0x10U
#define TCP_EPHEMERAL_FIRST 49152U
#define TCP_DEFAULT_MSS     536U
#define TCP_LOCAL_MSS       1460U
#define TCP_RTO_TICKS       TIMER_HZ
#define TCP_RTO_MIN         (TIMER_HZ / 5U)
#define TCP_RTO_MAX         (60U * TIMER_HZ)
#define TCP_PERSIST_MIN     TIMER_HZ
#define TCP_TIME_WAIT_TICKS (60U * TIMER_HZ)

typedef struct tcp_tx_record {
        struct tcp_tx_record *next;
        uint32_t              sequence;
        uint32_t              end_sequence;
        uint16_t              length;
        uint8_t               flags;
        uint8_t               retries;
        uint64_t              deadline;
        uint64_t              sent_at;
        uint8_t               retransmitted;
        uint8_t               data[];
} tcp_tx_record_t;

typedef struct tcp_ooo_record {
        struct tcp_ooo_record *next;
        uint32_t               sequence;
        uint16_t               length;
        uint8_t                fin;
        uint8_t                data[];
} tcp_ooo_record_t;

typedef struct tcp_endpoint {
        uint16_t             family;
        uint8_t              native6;
        uint8_t              v6only;
        uint32_t             local_address;
        uint32_t             remote_address;
        ipv6_address_t       local_address6;
        ipv6_address_t       remote_address6;
        uint16_t             local_port;
        uint16_t             remote_port;
        tcp_state_t          state;
        uint32_t             snd_una;
        uint32_t             snd_nxt;
        uint32_t             snd_wl1;
        uint32_t             snd_wl2;
        uint32_t             rcv_nxt;
        uint16_t             peer_window;
        uint16_t             peer_mss;
        uint16_t             rx_length;
        uint16_t             ooo_length;
        uint8_t             *rx_data;
        int                  error;
        uint8_t              bound;
        uint8_t              backlog;
        uint8_t              accept_head;
        uint8_t              accept_tail;
        uint8_t              accept_count;
        uint64_t             time_wait_until;
        uint64_t             last_received;
        uint64_t             keepalive_deadline;
        uint64_t             persist_deadline;
        uint32_t             cwnd;
        uint32_t             ssthresh;
        uint32_t             rto;
        uint32_t             srtt;
        uint32_t             rttvar;
        uint32_t             last_ack;
        uint32_t             recover;
        uint32_t             retransmissions;
        uint32_t             keepalive_probes_sent;
        uint32_t             persist_probes_sent;
        uint32_t             persist_interval;
        uint32_t             keepalive_idle;
        uint32_t             keepalive_interval;
        uint8_t              duplicate_acks;
        uint8_t              keepalive_probe_count;
        uint8_t              keepalive_count;
        uint8_t              syn_retries;
        uint8_t              data_retries;
        uint8_t              keepalive_enabled;
        uint8_t              fast_recovery;
        uint8_t              persist_needed;
        uint8_t              persist_byte;
        uint8_t              ooo_count;
        uint8_t              orphaned;
        struct tcp_endpoint *parent;
        struct tcp_endpoint *accept_queue[TCP_ACCEPT_MAX];
        tcp_tx_record_t     *tx_head;
        tcp_ooo_record_t    *ooo_head;
        wait_queue_t         wait;
        spinlock_t           lock;
        tcp_event_callback_t event_callback;
        void                *event_context;
} tcp_endpoint_t;

static tcp_endpoint_t *tcp_table[TCP_ENDPOINT_MAX];
static spinlock_t      tcp_table_lock;
static spinlock_t      tcp_iss_lock;
static uint16_t        tcp_ephemeral = TCP_EPHEMERAL_FIRST;
static uint32_t        tcp_iss_counter;

static int  tcp_emit(tcp_endpoint_t *endpoint, uint32_t sequence, uint32_t acknowledgment, uint8_t flags, const void *data, size_t length,
                     int track);
static int  tcp_autobind(tcp_endpoint_t *endpoint, uint32_t address);
static void tcp_records_free(tcp_tx_record_t *record);

static int seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}
static int seq_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

int net_tcp_seq_before(uint32_t a, uint32_t b)
{
    return seq_before(a, b);
}
int net_tcp_seq_after(uint32_t a, uint32_t b)
{
    return seq_after(a, b);
}

tcp_state_t net_tcp_state_next(tcp_state_t state, tcp_event_t event)
{
    if (event == TCP_EVENT_RX_RST || event == TCP_EVENT_TIMEOUT) return TCP_CLOSED;
    if (state == TCP_CLOSED && event == TCP_EVENT_ACTIVE_OPEN) return TCP_SYN_SENT;
    if (state == TCP_LISTEN && event == TCP_EVENT_RX_SYN) return TCP_SYN_RECEIVED;
    if (state == TCP_SYN_SENT && event == TCP_EVENT_RX_SYN_ACK) return TCP_ESTABLISHED;
    if (state == TCP_SYN_RECEIVED && event == TCP_EVENT_RX_ACK) return TCP_ESTABLISHED;
    if (state == TCP_ESTABLISHED && event == TCP_EVENT_CLOSE) return TCP_FIN_WAIT_1;
    if (state == TCP_ESTABLISHED && event == TCP_EVENT_RX_FIN) return TCP_CLOSE_WAIT;
    if (state == TCP_FIN_WAIT_1 && event == TCP_EVENT_RX_ACK) return TCP_FIN_WAIT_2;
    if (state == TCP_FIN_WAIT_1 && event == TCP_EVENT_RX_FIN) return TCP_CLOSING;
    if (state == TCP_FIN_WAIT_2 && event == TCP_EVENT_RX_FIN) return TCP_TIME_WAIT;
    if (state == TCP_CLOSE_WAIT && event == TCP_EVENT_CLOSE) return TCP_LAST_ACK;
    if (state == TCP_CLOSING && event == TCP_EVENT_RX_ACK) return TCP_TIME_WAIT;
    if (state == TCP_LAST_ACK && event == TCP_EVENT_RX_ACK) return TCP_CLOSED;
    return state;
}

int net_tcp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_tcp_segment_t *segment)
{
    if (!data || !segment || length < TCP_HEADER_LEN) return -EBADMSG;
    const uint8_t *bytes         = data;
    size_t         header_length = (size_t)(bytes[12] >> 4) * 4U;
    if (header_length < TCP_HEADER_LEN || header_length > length
        || net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_TCP, bytes, length) != 0)
        return -EBADMSG;
    segment->source_port      = net_read_be16(bytes);
    segment->destination_port = net_read_be16(bytes + 2);
    segment->sequence         = net_read_be32(bytes + 4);
    segment->acknowledgment   = net_read_be32(bytes + 8);
    segment->header_len       = (uint8_t)header_length;
    segment->flags            = bytes[13];
    segment->payload          = bytes + header_length;
    segment->payload_len      = length - header_length;
    return 0;
}

int net_tcp_parse6(const void *data, size_t length, const struct in6_addr *source, const struct in6_addr *destination,
                   net_tcp_segment_t *segment)
{
    if (!data || !source || !destination || !segment || length < TCP_HEADER_LEN) return -EBADMSG;
    const uint8_t        *bytes         = data;
    size_t                header_length = (size_t)(bytes[12] >> 4) * 4U;
    const ipv6_address_t *src           = (const ipv6_address_t *)source->s6_addr;
    const ipv6_address_t *dst           = (const ipv6_address_t *)destination->s6_addr;
    if (header_length < TCP_HEADER_LEN || header_length > length || net_checksum_ipv6_pseudo(src, dst, IPV6_NEXT_TCP, data, length) != 0)
        return -EBADMSG;
    segment->source_port      = net_read_be16(bytes);
    segment->destination_port = net_read_be16(bytes + 2);
    segment->sequence         = net_read_be32(bytes + 4);
    segment->acknowledgment   = net_read_be32(bytes + 8);
    segment->header_len       = (uint8_t)header_length;
    segment->flags            = bytes[13];
    segment->payload          = bytes + header_length;
    segment->payload_len      = length - header_length;
    return 0;
}

static uint16_t tcp_window(const tcp_endpoint_t *endpoint)
{
    return (uint16_t)(TCP_RX_BUFFER_MAX - endpoint->rx_length - endpoint->ooo_length);
}

static unsigned tcp_tx_count(const tcp_endpoint_t *endpoint)
{
    unsigned count = 0;
    for (const tcp_tx_record_t *record = endpoint->tx_head; record; record = record->next) count++;
    return count;
}

static uint32_t tcp_ready_locked(const tcp_endpoint_t *endpoint)
{
    uint32_t ready = 0;
    if (endpoint->state == TCP_LISTEN) return endpoint->accept_count ? TCP_READY_ACCEPT | TCP_READY_READ : 0;
    if (endpoint->rx_length || endpoint->state == TCP_CLOSE_WAIT || endpoint->state == TCP_CLOSED || endpoint->state == TCP_TIME_WAIT)
        ready |= TCP_READY_READ;
    if (endpoint->state == TCP_ESTABLISHED || endpoint->state == TCP_CLOSE_WAIT) {
        uint32_t flight = endpoint->snd_nxt - endpoint->snd_una;
        uint32_t limit  = endpoint->peer_window < endpoint->cwnd ? endpoint->peer_window : endpoint->cwnd;
        if (!endpoint->error && tcp_tx_count(endpoint) < TCP_TX_SEGMENT_MAX && flight < limit) ready |= TCP_READY_WRITE;
    }
    if (endpoint->error) ready |= TCP_READY_ERROR;
    if (endpoint->state == TCP_CLOSE_WAIT || endpoint->state == TCP_CLOSED || endpoint->state == TCP_TIME_WAIT) ready |= TCP_READY_HANGUP;
    return ready;
}

static void tcp_notify(tcp_endpoint_t *endpoint, uint32_t events)
{
    wait_queue_wake_all(&endpoint->wait);
    tcp_event_callback_t callback = endpoint->event_callback;
    void                *context  = endpoint->event_context;
    if (callback) callback(endpoint, events, context);
}

static void tcp_fail_locked(tcp_endpoint_t *endpoint, int error)
{
    endpoint->state = TCP_CLOSED;
    endpoint->error = error;
    tcp_records_free(endpoint->tx_head);
    endpoint->tx_head            = NULL;
    endpoint->persist_deadline   = 0;
    endpoint->keepalive_deadline = 0;
}

static uint32_t tcp_new_iss(void)
{
    spin_lock(&tcp_iss_lock);
    uint32_t iss = (uint32_t)sched_ticks() * 64000U + (tcp_iss_counter += 64001U);
    spin_unlock(&tcp_iss_lock);
    return iss;
}

static int tcp_port_used_locked(uint32_t address, uint16_t port, const tcp_endpoint_t *ignore)
{
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *ep = tcp_table[i];
        if (ep && ep != ignore && ep->bound && ep->local_port == port && (!ep->local_address || !address || ep->local_address == address))
            return 1;
    }
    return 0;
}

static int tcp_port_used6_locked(const ipv6_address_t *address, uint16_t port, const tcp_endpoint_t *ignore)
{
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *ep = tcp_table[i];
        if (!ep || ep == ignore || !ep->bound || ep->local_port != port) continue;
        if (ipv6_address_is_unspecified(&ep->local_address6) || ipv6_address_is_unspecified(address)
            || ipv6_address_equal(&ep->local_address6, address))
            return 1;
    }
    return 0;
}

static int tcp_insert_locked(tcp_endpoint_t *endpoint)
{
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        if (!tcp_table[i]) {
            tcp_table[i] = endpoint;
            return 0;
        }
    }
    return -ENOSPC;
}

static tcp_endpoint_t *tcp_alloc_locked(void)
{
    tcp_endpoint_t *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) {
        plogk("tcp: PCB alloc failed.\n");
        return NULL;
    }
    endpoint->rx_data = malloc(TCP_RX_BUFFER_MAX);
    if (!endpoint->rx_data) {
        plogk("tcp: RX buffer alloc failed (%u bytes).\n", (unsigned)TCP_RX_BUFFER_MAX);
        free(endpoint);
        return NULL;
    }
    endpoint->state              = TCP_CLOSED;
    endpoint->peer_window        = UINT16_MAX;
    endpoint->peer_mss           = TCP_DEFAULT_MSS;
    endpoint->cwnd               = TCP_LOCAL_MSS;
    endpoint->ssthresh           = UINT16_MAX;
    endpoint->rto                = TCP_RTO_TICKS;
    endpoint->keepalive_idle     = TCP_KEEPIDLE_DEFAULT_TICKS;
    endpoint->keepalive_interval = TCP_KEEPINTVL_DEFAULT_TICKS;
    endpoint->keepalive_count    = TCP_KEEPCNT_DEFAULT;
    endpoint->syn_retries        = TCP_SYN_RETRIES_DEFAULT;
    endpoint->data_retries       = TCP_DATA_RETRIES_DEFAULT;
    wait_queue_init(&endpoint->wait);
    if (tcp_insert_locked(endpoint)) {
        plogk("tcp: PCB table full.\n");
        free(endpoint->rx_data);
        free(endpoint);
        return NULL;
    }
    return endpoint;
}

tcp_endpoint_t *tcp_open_family(uint16_t family)
{
    spin_lock(&tcp_table_lock);
    tcp_endpoint_t *endpoint = tcp_alloc_locked();
    if (endpoint) endpoint->family = family;
    spin_unlock(&tcp_table_lock);
    return endpoint;
}

tcp_endpoint_t *tcp_open(void)
{
    return tcp_open_family(AF_INET);
}

static void tcp_records_free(tcp_tx_record_t *record)
{
    while (record) {
        tcp_tx_record_t *next = record->next;
        free(record);
        record = next;
    }
}

static void tcp_ooo_free(tcp_ooo_record_t *record)
{
    while (record) {
        tcp_ooo_record_t *next = record->next;
        free(record);
        record = next;
    }
}

void tcp_close(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return;
    spin_lock(&endpoint->lock);
    endpoint->event_callback = NULL;
    endpoint->event_context  = NULL;
    if (endpoint->orphaned) {
        spin_unlock(&endpoint->lock);
        return;
    }
    if (endpoint->state == TCP_CLOSED) {
        endpoint->orphaned        = 1;
        tcp_tx_record_t  *records = endpoint->tx_head;
        tcp_ooo_record_t *ooo     = endpoint->ooo_head;
        endpoint->tx_head         = NULL;
        endpoint->ooo_head        = NULL;
        spin_unlock(&endpoint->lock);
        spin_lock(&tcp_table_lock);
        for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++)
            if (tcp_table[i] == endpoint) tcp_table[i] = NULL;
        spin_unlock(&tcp_table_lock);
        if (endpoint->parent) {
            tcp_endpoint_t *parent = endpoint->parent;
            spin_lock(&parent->lock);
            for (unsigned i = 0; i < TCP_ACCEPT_MAX; i++) {
                if (parent->accept_queue[i] != endpoint) continue;
                parent->accept_queue[i] = NULL;
                if (parent->accept_count) parent->accept_count--;
            }
            spin_unlock(&parent->lock);
        }
        wait_queue_wake_all(&endpoint->wait);
        tcp_records_free(records);
        tcp_ooo_free(ooo);
        free(endpoint->rx_data);
        free(endpoint);
        return;
    }
    if (endpoint->state == TCP_ESTABLISHED || endpoint->state == TCP_CLOSE_WAIT) {
        tcp_state_t next = endpoint->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
        if (!tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0, 1)) {
            endpoint->snd_nxt++;
            endpoint->state    = next;
            endpoint->orphaned = 1;
            spin_unlock(&endpoint->lock);
            return;
        }
    }
    endpoint->state           = TCP_CLOSED;
    endpoint->orphaned        = 0;
    tcp_tx_record_t  *records = endpoint->tx_head;
    tcp_ooo_record_t *ooo     = endpoint->ooo_head;
    endpoint->tx_head         = NULL;
    endpoint->ooo_head        = NULL;
    spin_unlock(&endpoint->lock);
    spin_lock(&tcp_table_lock);
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++)
        if (tcp_table[i] == endpoint) tcp_table[i] = NULL;
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *child = tcp_table[i];
        if (!child || child->parent != endpoint) continue;
        for (unsigned j = 0; j < TCP_ACCEPT_MAX; j++) {
            if (endpoint->accept_queue[j] == child) {
                endpoint->accept_queue[j] = NULL;
                if (endpoint->accept_count) endpoint->accept_count--;
                break;
            }
        }
        tcp_table[i] = NULL;
        tcp_records_free(child->tx_head);
        tcp_ooo_free(child->ooo_head);
        free(child->rx_data);
        free(child);
    }
    spin_unlock(&tcp_table_lock);
    if (endpoint->parent) {
        tcp_endpoint_t *parent = endpoint->parent;
        spin_lock(&parent->lock);
        for (unsigned i = 0; i < TCP_ACCEPT_MAX; i++) {
            if (parent->accept_queue[i] != endpoint) continue;
            parent->accept_queue[i] = NULL;
            if (parent->accept_count) parent->accept_count--;
        }
        spin_unlock(&parent->lock);
    }
    memset(endpoint->accept_queue, 0, sizeof(endpoint->accept_queue));
    wait_queue_wake_all(&endpoint->wait);
    tcp_records_free(records);
    tcp_ooo_free(ooo);
    free(endpoint->rx_data);
    free(endpoint);
}

int tcp_bind(tcp_endpoint_t *endpoint, uint32_t address, uint16_t port)
{
    if (!endpoint) return -EINVAL;
    if (!port) return tcp_autobind(endpoint, address);
    spin_lock(&tcp_table_lock);
    if (endpoint->bound || tcp_port_used_locked(address, port, endpoint)) {
        spin_unlock(&tcp_table_lock);
        return endpoint->bound ? -EINVAL : -EADDRINUSE;
    }
    endpoint->local_address = address;
    endpoint->local_port    = port;
    endpoint->bound         = 1;
    spin_unlock(&tcp_table_lock);
    return 0;
}

int tcp_bind6(tcp_endpoint_t *endpoint, const ipv6_address_t *address, uint16_t port)
{
    if (!endpoint || !address || endpoint->family != AF_INET6) return -EINVAL;
    endpoint->native6 = 1;
    if (!port) {
        int status = tcp_autobind(endpoint, 0);
        if (!status) endpoint->local_address6 = *address;
        return status;
    }
    spin_lock(&tcp_table_lock);
    if (endpoint->bound || tcp_port_used6_locked(address, port, endpoint)) {
        spin_unlock(&tcp_table_lock);
        return endpoint->bound ? -EINVAL : -EADDRINUSE;
    }
    endpoint->local_address6 = *address;
    endpoint->local_port     = port;
    endpoint->bound          = 1;
    spin_unlock(&tcp_table_lock);
    return 0;
}

static int tcp_autobind(tcp_endpoint_t *endpoint, uint32_t address)
{
    if (endpoint->bound) return 0;
    spin_lock(&tcp_table_lock);
    for (unsigned n = 0; n <= UINT16_MAX - TCP_EPHEMERAL_FIRST; n++) {
        uint16_t port = tcp_ephemeral++;
        if (tcp_ephemeral < TCP_EPHEMERAL_FIRST) tcp_ephemeral = TCP_EPHEMERAL_FIRST;
        if (!tcp_port_used_locked(address, port, endpoint)) {
            endpoint->local_address = address;
            endpoint->local_port    = port;
            endpoint->bound         = 1;
            spin_unlock(&tcp_table_lock);
            return 0;
        }
    }
    spin_unlock(&tcp_table_lock);
    return -EADDRINUSE;
}

int tcp_listen(tcp_endpoint_t *endpoint, unsigned backlog)
{
    if (!endpoint || !endpoint->bound || !backlog) return -EINVAL;
    spin_lock(&endpoint->lock);
    if (endpoint->state != TCP_CLOSED) {
        spin_unlock(&endpoint->lock);
        return -EINVAL;
    }
    endpoint->backlog = (uint8_t)(backlog > TCP_ACCEPT_MAX ? TCP_ACCEPT_MAX : backlog);
    endpoint->state   = TCP_LISTEN;
    spin_unlock(&endpoint->lock);
    return 0;
}

tcp_endpoint_t *tcp_accept(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return NULL;
    spin_lock(&endpoint->lock);
    tcp_endpoint_t *accepted = NULL;
    for (unsigned n = 0; n < TCP_ACCEPT_MAX; n++) {
        unsigned index = (endpoint->accept_head + n) % TCP_ACCEPT_MAX;
        if (endpoint->accept_queue[index]) {
            accepted                      = endpoint->accept_queue[index];
            endpoint->accept_queue[index] = NULL;
            endpoint->accept_head         = (uint8_t)((index + 1) % TCP_ACCEPT_MAX);
            endpoint->accept_count--;
            accepted->parent = NULL;
            break;
        }
    }
    spin_unlock(&endpoint->lock);
    return accepted;
}

static int tcp_emit(tcp_endpoint_t *endpoint, uint32_t sequence, uint32_t acknowledgment, uint8_t flags, const void *data, size_t length,
                    int track)
{
    size_t header_length = TCP_HEADER_LEN + ((flags & TCP_FLAG_SYN) ? 4U : 0U);
    if (length > UINT16_MAX - header_length) return -EMSGSIZE;
    net_device_t  *device;
    uint32_t       next_hop;
    ipv6_address_t source6, next_hop6;
    int            status = endpoint->native6 ? ipv6_route(&endpoint->remote_address6, &device, &source6, &next_hop6) :
                                                ipv4_route(endpoint->remote_address, &device, &next_hop);
    if (status) return status;
    net_pbuf_t *packet = net_pbuf_alloc(header_length + length, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("tcp: segment alloc failed (local=%u remote=%u len=%lu).\n", (unsigned)endpoint->local_port, (unsigned)endpoint->remote_port,
              (unsigned long)(header_length + length));
        netdev_put(device);
        return -ENOMEM;
    }
    uint8_t *tcp = packet->data;
    memset(tcp, 0, header_length);
    net_write_be16(tcp, endpoint->local_port);
    net_write_be16(tcp + 2, endpoint->remote_port);
    net_write_be32(tcp + 4, sequence);
    net_write_be32(tcp + 8, acknowledgment);
    tcp[12] = (uint8_t)((header_length / 4U) << 4);
    tcp[13] = flags;
    net_write_be16(tcp + 14, tcp_window(endpoint));
    if (flags & TCP_FLAG_SYN) {
        tcp[20] = 2;
        tcp[21] = 4;
        net_write_be16(tcp + 22, TCP_LOCAL_MSS);
    }
    if (length) memcpy(tcp + header_length, data, length);
    if (endpoint->native6 && ipv6_address_is_unspecified(&endpoint->local_address6)) endpoint->local_address6 = source6;
    uint16_t checksum = endpoint->native6 ?
                            net_checksum_ipv6_pseudo(&endpoint->local_address6, &endpoint->remote_address6, IPV6_NEXT_TCP, tcp, packet->length) :
                            net_checksum_ipv4_pseudo(endpoint->local_address, endpoint->remote_address, IPV4_PROTO_TCP, tcp, packet->length);
    net_write_be16(tcp + 16, checksum);
    tcp_tx_record_t *record          = NULL;
    uint32_t         sequence_length = (uint32_t)length + !!(flags & TCP_FLAG_SYN) + !!(flags & TCP_FLAG_FIN);
    if (track && sequence_length) {
        record = malloc(sizeof(*record) + length);
        if (!record) {
            plogk("tcp: TX record alloc failed (local=%u remote=%u len=%lu).\n", (unsigned)endpoint->local_port, (unsigned)endpoint->remote_port,
                  (unsigned long)length);
            net_pbuf_free(packet);
            netdev_put(device);
            return -ENOMEM;
        }
        record->next          = NULL;
        record->sequence      = sequence;
        record->end_sequence  = sequence + sequence_length;
        record->length        = (uint16_t)length;
        record->flags         = flags;
        record->retries       = 0;
        record->deadline      = sched_ticks() + endpoint->rto;
        record->sent_at       = sched_ticks();
        record->retransmitted = 0;
        if (length) memcpy(record->data, data, length);
    }
    status = endpoint->native6 ? ipv6_output(device, &endpoint->local_address6, &endpoint->remote_address6, IPV6_NEXT_TCP, 64, packet) :
                                 ipv4_output(device, endpoint->local_address, endpoint->remote_address, IPV4_PROTO_TCP, 64, packet);
    net_pbuf_free(packet);
    netdev_put(device);
    if (status && status != -EINPROGRESS) {
        free(record);
        return status;
    }
    if (record) {
        tcp_tx_record_t **tail = &endpoint->tx_head;
        while (*tail) tail = &(*tail)->next;
        *tail = record;
    }
    return 0;
}

int tcp_connect(tcp_endpoint_t *endpoint, uint32_t address, uint16_t port)
{
    if (!endpoint || !address || !port) return -EINVAL;
    net_device_t *device;
    uint32_t      next_hop;
    int           status = ipv4_route(address, &device, &next_hop);
    if (status) return status;
    status = tcp_autobind(endpoint, device->ipv4_address);
    netdev_put(device);
    if (status) return status;
    spin_lock(&endpoint->lock);
    if (endpoint->state != TCP_CLOSED) {
        spin_unlock(&endpoint->lock);
        return -EALREADY;
    }
    endpoint->remote_address = address;
    endpoint->native6        = 0;
    endpoint->remote_port    = port;
    endpoint->snd_una        = tcp_new_iss();
    endpoint->snd_nxt        = endpoint->snd_una + 1;
    endpoint->state          = TCP_SYN_SENT;
    endpoint->last_received  = sched_ticks();
    endpoint->error          = 0;
    status                   = tcp_emit(endpoint, endpoint->snd_una, 0, TCP_FLAG_SYN, NULL, 0, 1);
    if (status) endpoint->state = TCP_CLOSED;
    spin_unlock(&endpoint->lock);
    return status ? status : -EINPROGRESS;
}

int tcp_connect6(tcp_endpoint_t *endpoint, const ipv6_address_t *address, uint16_t port)
{
    if (!endpoint || !address || endpoint->family != AF_INET6 || ipv6_address_is_unspecified(address) || !port) return -EINVAL;
    net_device_t  *device;
    ipv6_address_t source, next_hop;
    int            status = ipv6_route(address, &device, &source, &next_hop);
    if (status) return status;
    status = tcp_autobind(endpoint, 0);
    netdev_put(device);
    if (status) return status;
    spin_lock(&endpoint->lock);
    if (endpoint->state != TCP_CLOSED) {
        spin_unlock(&endpoint->lock);
        return -EALREADY;
    }
    if (ipv6_address_is_unspecified(&endpoint->local_address6)) endpoint->local_address6 = source;
    endpoint->native6         = 1;
    endpoint->remote_address6 = *address;
    endpoint->remote_port     = port;
    endpoint->snd_una         = tcp_new_iss();
    endpoint->snd_nxt         = endpoint->snd_una + 1;
    endpoint->state           = TCP_SYN_SENT;
    endpoint->last_received   = sched_ticks();
    endpoint->error           = 0;
    status                    = tcp_emit(endpoint, endpoint->snd_una, 0, TCP_FLAG_SYN, NULL, 0, 1);
    if (status) endpoint->state = TCP_CLOSED;
    spin_unlock(&endpoint->lock);
    return status ? status : -EINPROGRESS;
}

int tcp_send(tcp_endpoint_t *endpoint, const void *data, size_t length)
{
    if (!endpoint || (!data && length)) return -EINVAL;
    if (!length) return 0;
    spin_lock(&endpoint->lock);
    if (endpoint->error) {
        int error = endpoint->error;
        spin_unlock(&endpoint->lock);
        return -error;
    }
    if (endpoint->state != TCP_ESTABLISHED && endpoint->state != TCP_CLOSE_WAIT) {
        spin_unlock(&endpoint->lock);
        return -ENOTCONN;
    }
    size_t sent = 0;
    while (sent < length) {
        unsigned records     = tcp_tx_count(endpoint);
        uint32_t flight      = endpoint->snd_nxt - endpoint->snd_una;
        uint32_t send_window = endpoint->peer_window < endpoint->cwnd ? endpoint->peer_window : endpoint->cwnd;
        if (records >= TCP_TX_SEGMENT_MAX || send_window <= flight) {
            if (!endpoint->peer_window) {
                if (!endpoint->tx_head && !endpoint->persist_needed && sent < length) {
                    endpoint->persist_byte   = ((const uint8_t *)data)[sent];
                    endpoint->persist_needed = 1;
                }
                if (!endpoint->persist_deadline) {
                    endpoint->persist_interval = endpoint->rto > TCP_PERSIST_MIN ? endpoint->rto : TCP_PERSIST_MIN;
                    endpoint->persist_deadline = sched_ticks() + endpoint->persist_interval;
                }
            }
            break;
        }
        size_t allowed = send_window - flight;
        size_t chunk   = length - sent;
        if (chunk > endpoint->peer_mss) chunk = endpoint->peer_mss;
        if (chunk > allowed) chunk = allowed;
        if (!chunk) break;
        int status
            = tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH, (const uint8_t *)data + sent, chunk, 1);
        if (status) {
            spin_unlock(&endpoint->lock);
            return sent ? (int)sent : status;
        }
        endpoint->snd_nxt += (uint32_t)chunk;
        sent += chunk;
    }
    spin_unlock(&endpoint->lock);
    return sent ? (int)sent : -EAGAIN;
}

int tcp_receive(tcp_endpoint_t *endpoint, void *data, size_t capacity)
{
    if (!endpoint || (!data && capacity)) return -EINVAL;
    spin_lock(&endpoint->lock);
    if (!endpoint->rx_length) {
        int result = (endpoint->state == TCP_CLOSE_WAIT || endpoint->state == TCP_CLOSED || endpoint->state == TCP_TIME_WAIT) ? 0 : -EAGAIN;
        spin_unlock(&endpoint->lock);
        return result;
    }
    size_t copied = endpoint->rx_length < capacity ? endpoint->rx_length : capacity;
    if (copied) {
        memcpy(data, endpoint->rx_data, copied);
        memmove(endpoint->rx_data, endpoint->rx_data + copied, endpoint->rx_length - copied);
        endpoint->rx_length -= (uint16_t)copied;
        if (tcp_window(endpoint) > copied) tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
    }
    spin_unlock(&endpoint->lock);
    return (int)copied;
}

int tcp_shutdown(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    spin_lock(&endpoint->lock);
    tcp_state_t next;
    if (endpoint->state == TCP_ESTABLISHED)
        next = TCP_FIN_WAIT_1;
    else if (endpoint->state == TCP_CLOSE_WAIT)
        next = TCP_LAST_ACK;
    else {
        spin_unlock(&endpoint->lock);
        return -ENOTCONN;
    }
    int status = tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0, 1);
    if (!status) {
        endpoint->snd_nxt++;
        endpoint->state = next;
    }
    spin_unlock(&endpoint->lock);
    return status;
}

tcp_state_t tcp_get_state(const tcp_endpoint_t *endpoint)
{
    if (!endpoint) return TCP_CLOSED;
    tcp_endpoint_t *mutable = (tcp_endpoint_t *)endpoint;
    spin_lock(&mutable->lock);
    tcp_state_t state = mutable->state;
    spin_unlock(&mutable->lock);
    return state;
}

int tcp_get_error(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return EINVAL;
    spin_lock(&endpoint->lock);
    int error       = endpoint->error;
    endpoint->error = 0;
    spin_unlock(&endpoint->lock);
    return error;
}

int tcp_set_option(tcp_endpoint_t *endpoint, tcp_option_t option, uint32_t value)
{
    if (!endpoint) return -EINVAL;
    spin_lock(&endpoint->lock);
    int status = 0;
    switch (option) {
        case TCP_OPTION_KEEPALIVE :
            endpoint->keepalive_enabled     = value != 0;
            endpoint->keepalive_probe_count = 0;
            endpoint->keepalive_deadline    = endpoint->keepalive_enabled ? sched_ticks() + endpoint->keepalive_idle : 0;
            break;
        case TCP_OPTION_KEEPIDLE_TICKS :
            if (!value)
                status = -EINVAL;
            else
                endpoint->keepalive_idle = value;
            break;
        case TCP_OPTION_KEEPINTVL_TICKS :
            if (!value)
                status = -EINVAL;
            else
                endpoint->keepalive_interval = value;
            break;
        case TCP_OPTION_KEEPCNT :
            if (!value || value > UINT8_MAX)
                status = -EINVAL;
            else
                endpoint->keepalive_count = (uint8_t)value;
            break;
        case TCP_OPTION_SYN_RETRIES :
            if (!value || value > UINT8_MAX)
                status = -EINVAL;
            else
                endpoint->syn_retries = (uint8_t)value;
            break;
        case TCP_OPTION_DATA_RETRIES :
            if (!value || value > UINT8_MAX)
                status = -EINVAL;
            else
                endpoint->data_retries = (uint8_t)value;
            break;
        default :
            status = -ENOPROTOOPT;
            break;
    }
    if (!status && endpoint->keepalive_enabled && (option == TCP_OPTION_KEEPIDLE_TICKS || option == TCP_OPTION_KEEPINTVL_TICKS)) {
        endpoint->keepalive_probe_count = 0;
        endpoint->keepalive_deadline    = sched_ticks() + endpoint->keepalive_idle;
    }
    spin_unlock(&endpoint->lock);
    return status;
}

int tcp_get_option(tcp_endpoint_t *endpoint, tcp_option_t option, uint32_t *value)
{
    if (!endpoint || !value) return -EINVAL;
    spin_lock(&endpoint->lock);
    int status = 0;
    switch (option) {
        case TCP_OPTION_KEEPALIVE :
            *value = endpoint->keepalive_enabled;
            break;
        case TCP_OPTION_KEEPIDLE_TICKS :
            *value = endpoint->keepalive_idle;
            break;
        case TCP_OPTION_KEEPINTVL_TICKS :
            *value = endpoint->keepalive_interval;
            break;
        case TCP_OPTION_KEEPCNT :
            *value = endpoint->keepalive_count;
            break;
        case TCP_OPTION_SYN_RETRIES :
            *value = endpoint->syn_retries;
            break;
        case TCP_OPTION_DATA_RETRIES :
            *value = endpoint->data_retries;
            break;
        default :
            status = -ENOPROTOOPT;
            break;
    }
    spin_unlock(&endpoint->lock);
    return status;
}

static void tcp_trim_acked_record(tcp_tx_record_t *record, uint32_t acknowledgment)
{
    uint32_t consumed = acknowledgment - record->sequence;
    if ((record->flags & TCP_FLAG_SYN) && consumed) {
        record->flags &= (uint8_t)~TCP_FLAG_SYN;
        record->sequence++;
        consumed--;
    }
    if (consumed) {
        size_t data_acked = consumed < record->length ? consumed : record->length;
        memmove(record->data, record->data + data_acked, record->length - data_acked);
        record->length -= (uint16_t)data_acked;
        record->sequence += (uint32_t)data_acked;
    }
}

static void tcp_ack_records(tcp_endpoint_t *endpoint, uint32_t acknowledgment)
{
    uint32_t newly_acked = acknowledgment - endpoint->snd_una;
    uint64_t now         = sched_ticks();
    while (endpoint->tx_head && !seq_before(acknowledgment, endpoint->tx_head->end_sequence)) {
        tcp_tx_record_t *record = endpoint->tx_head;
        endpoint->tx_head       = record->next;
        if (!record->retransmitted) {
            uint32_t sample = (uint32_t)(now - record->sent_at);
            if (!sample) sample = 1;
            if (!endpoint->srtt) {
                endpoint->srtt   = sample << 3;
                endpoint->rttvar = sample << 1;
            } else {
                int32_t error = (int32_t)sample - (int32_t)(endpoint->srtt >> 3);
                endpoint->srtt += error;
                if (error < 0) error = -error;
                endpoint->rttvar = (uint32_t)((int32_t)endpoint->rttvar + (error - (int32_t)(endpoint->rttvar >> 2)));
            }
            uint32_t rto  = (endpoint->srtt >> 3) + endpoint->rttvar;
            endpoint->rto = rto < TCP_RTO_MIN ? TCP_RTO_MIN : (rto > TCP_RTO_MAX ? TCP_RTO_MAX : rto);
        }
        free(record);
    }
    if (endpoint->tx_head && seq_after(acknowledgment, endpoint->tx_head->sequence)) tcp_trim_acked_record(endpoint->tx_head, acknowledgment);
    endpoint->snd_una = acknowledgment;
    if (newly_acked) {
        if (endpoint->fast_recovery) {
            if (!seq_before(acknowledgment, endpoint->recover)) {
                endpoint->fast_recovery = 0;
                endpoint->cwnd          = endpoint->ssthresh;
            } else {
                endpoint->cwnd          = endpoint->ssthresh + endpoint->peer_mss;
                tcp_tx_record_t *record = endpoint->tx_head;
                if (record) {
                    tcp_emit(endpoint, record->sequence, endpoint->rcv_nxt, record->flags, record->data, record->length, 0);
                    record->retransmitted = 1;
                    record->deadline      = now + endpoint->rto;
                    endpoint->retransmissions++;
                }
            }
        } else if (endpoint->cwnd < endpoint->ssthresh)
            endpoint->cwnd += newly_acked < endpoint->peer_mss ? newly_acked : endpoint->peer_mss;
        else {
            uint32_t increase = endpoint->peer_mss * endpoint->peer_mss / endpoint->cwnd;
            endpoint->cwnd += increase ? increase : 1;
        }
        endpoint->duplicate_acks = 0;
        endpoint->last_ack       = acknowledgment;
    }
}

static uint16_t tcp_parse_mss(const uint8_t *tcp, size_t header_length)
{
    size_t offset = TCP_HEADER_LEN;
    while (offset < header_length) {
        uint8_t kind = tcp[offset];
        if (!kind) break;
        if (kind == 1) {
            offset++;
            continue;
        }
        if (offset + 2 > header_length || tcp[offset + 1] < 2 || offset + tcp[offset + 1] > header_length) break;
        if (kind == 2 && tcp[offset + 1] == 4) {
            uint16_t mss = net_read_be16(tcp + offset + 2);
            if (mss < TCP_DEFAULT_MSS) return TCP_DEFAULT_MSS;
            return mss > TCP_LOCAL_MSS ? TCP_LOCAL_MSS : mss;
        }
        offset += tcp[offset + 1];
    }
    return TCP_DEFAULT_MSS;
}

static int tcp_queue_ooo(tcp_endpoint_t *endpoint, uint32_t sequence, const uint8_t *data, size_t length, int fin)
{
    if ((!length && !fin) || endpoint->ooo_count >= TCP_OOO_SEGMENT_MAX || length > UINT16_MAX
        || length > TCP_RX_BUFFER_MAX - endpoint->rx_length - endpoint->ooo_length)
        return -ENOBUFS;
    tcp_ooo_record_t **position = &endpoint->ooo_head;
    while (*position && seq_before((*position)->sequence, sequence)) position = &(*position)->next;
    if (*position && (*position)->sequence == sequence) return 0;
    if (position != &endpoint->ooo_head) {
        tcp_ooo_record_t *previous = endpoint->ooo_head;
        while (previous->next != *position) previous = previous->next;
        if (seq_after(previous->sequence + previous->length + previous->fin, sequence)) return 0;
    }
    if (*position && seq_after(sequence + (uint32_t)length + fin, (*position)->sequence)) return 0;
    tcp_ooo_record_t *record = malloc(sizeof(*record) + length);
    if (!record) {
        plogk("tcp: OOO record alloc failed (local=%u remote=%u len=%lu).\n", (unsigned)endpoint->local_port, (unsigned)endpoint->remote_port,
              (unsigned long)length);
        return -ENOMEM;
    }
    record->next     = *position;
    record->sequence = sequence;
    record->length   = (uint16_t)length;
    record->fin      = (uint8_t)fin;
    if (length) memcpy(record->data, data, length);
    *position = record;
    endpoint->ooo_count++;
    endpoint->ooo_length += (uint16_t)length;
    return 0;
}

static void tcp_received_fin(tcp_endpoint_t *endpoint)
{
    endpoint->rcv_nxt++;
    if (endpoint->state == TCP_ESTABLISHED)
        endpoint->state = TCP_CLOSE_WAIT;
    else if (endpoint->state == TCP_FIN_WAIT_1)
        endpoint->state = TCP_CLOSING;
    else if (endpoint->state == TCP_FIN_WAIT_2) {
        endpoint->state           = TCP_TIME_WAIT;
        endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
    }
}

static void tcp_drain_ooo(tcp_endpoint_t *endpoint)
{
    while (endpoint->ooo_head && endpoint->ooo_head->sequence == endpoint->rcv_nxt) {
        tcp_ooo_record_t *record = endpoint->ooo_head;
        if (record->length > TCP_RX_BUFFER_MAX - endpoint->rx_length) break;
        endpoint->ooo_head = record->next;
        endpoint->ooo_count--;
        endpoint->ooo_length -= record->length;
        if (record->length) {
            memcpy(endpoint->rx_data + endpoint->rx_length, record->data, record->length);
            endpoint->rx_length += record->length;
            endpoint->rcv_nxt += record->length;
        }
        if (record->fin) tcp_received_fin(endpoint);
        free(record);
    }
}

static tcp_endpoint_t *tcp_lookup_locked(const ipv4_info_t *ip, uint16_t source_port, uint16_t destination_port, tcp_endpoint_t **listener)
{
    *listener = NULL;
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *ep = tcp_table[i];
        if (!ep || (ep->family != AF_INET && (ep->family != AF_INET6 || ep->v6only || !ipv6_address_is_unspecified(&ep->local_address6)))
            || !ep->bound || ep->local_port != destination_port || (ep->local_address && ep->local_address != ip->destination))
            continue;
        if (ep->state == TCP_LISTEN)
            *listener = ep;
        else if (ep->remote_address == ip->source && ep->remote_port == source_port)
            return ep;
    }
    return NULL;
}

static tcp_endpoint_t *tcp_lookup6_locked(const ipv6_info_t *ip, uint16_t source_port, uint16_t destination_port, tcp_endpoint_t **listener)
{
    *listener = NULL;
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *ep = tcp_table[i];
        if (!ep || ep->family != AF_INET6 || !ep->bound || ep->local_port != destination_port
            || (!ipv6_address_is_unspecified(&ep->local_address6) && !ipv6_address_equal(&ep->local_address6, &ip->destination)))
            continue;
        if (ep->state == TCP_LISTEN)
            *listener = ep;
        else if (ipv6_address_equal(&ep->remote_address6, &ip->source) && ep->remote_port == source_port)
            return ep;
    }
    return NULL;
}

static int tcp_reset_reply(const ipv4_info_t *ip, uint16_t source_port, uint16_t destination_port, uint32_t sequence, uint32_t acknowledgment,
                           uint8_t flags, size_t payload_length)
{
    if (flags & TCP_FLAG_RST) return 0;
    tcp_endpoint_t temporary;
    memset(&temporary, 0, sizeof(temporary));
    temporary.local_address  = ip->destination;
    temporary.remote_address = ip->source;
    temporary.local_port     = destination_port;
    temporary.remote_port    = source_port;
    if (flags & TCP_FLAG_ACK) return tcp_emit(&temporary, acknowledgment, 0, TCP_FLAG_RST, NULL, 0, 0);
    uint32_t ack = sequence + (uint32_t)payload_length + !!(flags & TCP_FLAG_SYN) + !!(flags & TCP_FLAG_FIN);
    return tcp_emit(&temporary, 0, ack, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0, 0);
}

static int tcp_passive_open(tcp_endpoint_t *listener, const ipv4_info_t *ip, uint16_t source_port, uint32_t sequence, uint16_t peer_mss)
{
    unsigned pending = 0;
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++)
        if (tcp_table[i] && tcp_table[i]->parent == listener) pending++;
    if (pending >= listener->backlog) return -ENOBUFS;
    tcp_endpoint_t *child = tcp_alloc_locked();
    if (!child) {
        plogk("tcp: passive open alloc failed (local port=%u peer=%u.%u.%u.%u:%u).\n", (unsigned)listener->local_port,
              (unsigned)(ip->source >> 24) & 0xff, (unsigned)(ip->source >> 16) & 0xff, (unsigned)(ip->source >> 8) & 0xff,
              (unsigned)ip->source & 0xff, (unsigned)source_port);
        return -ENOBUFS;
    }
    child->bound              = 1;
    child->family             = listener->family;
    child->native6            = 0;
    child->v6only             = listener->v6only;
    child->local_address      = ip->destination;
    child->local_port         = listener->local_port;
    child->remote_address     = ip->source;
    child->remote_port        = source_port;
    child->rcv_nxt            = sequence + 1;
    child->snd_una            = tcp_new_iss();
    child->snd_nxt            = child->snd_una + 1;
    child->state              = TCP_SYN_RECEIVED;
    child->peer_mss           = peer_mss;
    child->last_received      = sched_ticks();
    child->keepalive_idle     = listener->keepalive_idle;
    child->keepalive_interval = listener->keepalive_interval;
    child->keepalive_count    = listener->keepalive_count;
    child->syn_retries        = listener->syn_retries;
    child->data_retries       = listener->data_retries;
    child->keepalive_enabled  = listener->keepalive_enabled;
    child->parent             = listener;
    spin_lock(&child->lock);
    int status = tcp_emit(child, child->snd_una, child->rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0, 1);
    spin_unlock(&child->lock);
    if (status) {
        for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++)
            if (tcp_table[i] == child) tcp_table[i] = NULL;
        free(child->rx_data);
        free(child);
    }
    return status;
}

static int tcp_passive_open6(tcp_endpoint_t *listener, const ipv6_info_t *ip, uint16_t source_port, uint32_t sequence, uint16_t peer_mss)
{
    unsigned pending = 0;
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++)
        if (tcp_table[i] && tcp_table[i]->parent == listener) pending++;
    if (pending >= listener->backlog) return -ENOBUFS;
    tcp_endpoint_t *child = tcp_alloc_locked();
    if (!child) {
        plogk("tcp: passive open6 alloc failed (local port=%u peer=%u).\n", (unsigned)listener->local_port, (unsigned)source_port);
        return -ENOBUFS;
    }
    child->bound           = 1;
    child->family          = AF_INET6;
    child->native6         = 1;
    child->v6only          = listener->v6only;
    child->local_address6  = ip->destination;
    child->local_port      = listener->local_port;
    child->remote_address6 = ip->source;
    child->remote_port     = source_port;
    child->rcv_nxt         = sequence + 1;
    child->snd_una         = tcp_new_iss();
    child->snd_nxt         = child->snd_una + 1;
    child->state           = TCP_SYN_RECEIVED;
    child->peer_mss        = peer_mss;
    child->last_received   = sched_ticks();
    child->parent          = listener;
    spin_lock(&child->lock);
    int status = tcp_emit(child, child->snd_una, child->rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0, 1);
    spin_unlock(&child->lock);
    if (status) {
        for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++)
            if (tcp_table[i] == child) tcp_table[i] = NULL;
        free(child->rx_data);
        free(child);
    }
    return status;
}

int tcp_input6(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet)
{
    (void)device;
    if (!ip || !packet || packet->length < TCP_HEADER_LEN) goto bad;
    uint8_t *tcp           = packet->data;
    size_t   header_length = (size_t)(tcp[12] >> 4) * 4U;
    if (header_length < TCP_HEADER_LEN || header_length > packet->length
        || net_checksum_ipv6_pseudo(&ip->source, &ip->destination, IPV6_NEXT_TCP, tcp, packet->length) != 0)
        goto bad;
    uint16_t source_port      = net_read_be16(tcp);
    uint16_t destination_port = net_read_be16(tcp + 2);
    uint32_t sequence         = net_read_be32(tcp + 4);
    uint32_t acknowledgment   = net_read_be32(tcp + 8);
    uint8_t  flags            = tcp[13];
    uint16_t window           = net_read_be16(tcp + 14);
    size_t   payload_length   = packet->length - header_length;
    if (!source_port || !destination_port || (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) == (TCP_FLAG_SYN | TCP_FLAG_FIN)) goto bad;

    spin_lock(&tcp_table_lock);
    tcp_endpoint_t *listener;
    tcp_endpoint_t *endpoint = tcp_lookup6_locked(ip, source_port, destination_port, &listener);
    if (!endpoint) {
        if (listener && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
            spin_lock(&listener->lock);
            int status = tcp_passive_open6(listener, ip, source_port, sequence, tcp_parse_mss(tcp, header_length));
            spin_unlock(&listener->lock);
            spin_unlock(&tcp_table_lock);
            net_pbuf_free(packet);
            return status;
        }
        spin_unlock(&tcp_table_lock);
        net_pbuf_free(packet);
        return -ECONNREFUSED;
    }
    spin_lock(&endpoint->lock);
    spin_unlock(&tcp_table_lock);
    if (flags & TCP_FLAG_RST) {
        int acceptable = endpoint->state == TCP_SYN_SENT ?
                             ((flags & TCP_FLAG_ACK) && acknowledgment == endpoint->snd_nxt) :
                             (!seq_before(sequence, endpoint->rcv_nxt) && !seq_after(sequence, endpoint->rcv_nxt + tcp_window(endpoint)));
        if (!acceptable) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->state          = TCP_CLOSED;
        endpoint->error          = ECONNRESET;
        tcp_event_callback_t cb  = endpoint->event_callback;
        void                *ctx = endpoint->event_context;
        wait_queue_wake_all(&endpoint->wait);
        spin_unlock(&endpoint->lock);
        if (cb) cb(endpoint, TCP_READY_ERROR | TCP_READY_READ | TCP_READY_HANGUP, ctx);
        net_pbuf_free(packet);
        return -ECONNRESET;
    }
    endpoint->peer_window = window;
    if (endpoint->state == TCP_TIME_WAIT) {
        uint16_t receive_window = tcp_window(endpoint);
        int      acceptable     = receive_window ?
                                      (!seq_before(sequence, endpoint->rcv_nxt) && seq_before(sequence, endpoint->rcv_nxt + receive_window)) :
                                      sequence == endpoint->rcv_nxt;
        if (acceptable) {
            uint32_t segment_end = sequence + (uint32_t)payload_length + !!(flags & TCP_FLAG_FIN);
            if ((flags & TCP_FLAG_FIN) && segment_end == endpoint->rcv_nxt) endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
        }
        spin_unlock(&endpoint->lock);
        net_pbuf_free(packet);
        return 0;
    }
    uint64_t now6                   = sched_ticks();
    endpoint->last_received         = now6;
    endpoint->keepalive_probe_count = 0;
    endpoint->keepalive_deadline    = endpoint->keepalive_enabled ? now6 + endpoint->keepalive_idle : 0;
    if (flags & TCP_FLAG_ACK) {
        if (seq_before(acknowledgment, endpoint->snd_una) || seq_after(acknowledgment, endpoint->snd_nxt)) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EBADMSG;
        }
        if (seq_after(acknowledgment, endpoint->snd_una)) tcp_ack_records(endpoint, acknowledgment);
    }
    if (endpoint->state == TCP_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) != (TCP_FLAG_SYN | TCP_FLAG_ACK) || acknowledgment != endpoint->snd_nxt) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->rcv_nxt  = sequence + 1;
        endpoint->peer_mss = tcp_parse_mss(tcp, header_length);
        endpoint->state    = TCP_ESTABLISHED;
        tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
    } else if (endpoint->state == TCP_SYN_RECEIVED) {
        if (!(flags & TCP_FLAG_ACK) || acknowledgment != endpoint->snd_nxt || sequence != endpoint->rcv_nxt) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->state        = TCP_ESTABLISHED;
        tcp_endpoint_t *parent = endpoint->parent;
        if (parent) {
            spin_lock(&parent->lock);
            for (unsigned n = 0; n < TCP_ACCEPT_MAX; n++) {
                unsigned index = (parent->accept_tail + n) % TCP_ACCEPT_MAX;
                if (!parent->accept_queue[index]) {
                    parent->accept_queue[index] = endpoint;
                    parent->accept_tail         = (uint8_t)((index + 1) % TCP_ACCEPT_MAX);
                    parent->accept_count++;
                    break;
                }
            }
            tcp_event_callback_t cb_parent  = parent->event_callback;
            void                *ctx_parent = parent->event_context;
            wait_queue_wake_all(&parent->wait);
            spin_unlock(&parent->lock);
            if (cb_parent) cb_parent(parent, TCP_READY_ACCEPT | TCP_READY_READ, ctx_parent);
        }
    } else if (endpoint->state != TCP_ESTABLISHED && endpoint->state != TCP_FIN_WAIT_1 && endpoint->state != TCP_FIN_WAIT_2
               && endpoint->state != TCP_CLOSE_WAIT && endpoint->state != TCP_CLOSING && endpoint->state != TCP_LAST_ACK) {
        plogk("tcp: segment on closed connection (local=%u remote=%u:%u).\n", (unsigned)endpoint->local_port, (unsigned)endpoint->remote_port,
              (unsigned)source_port);
        spin_unlock(&endpoint->lock);
        net_pbuf_free(packet);
        return -ENOTCONN;
    } else if (endpoint->state != TCP_SYN_SENT && endpoint->state != TCP_SYN_RECEIVED) {
        uint16_t receive_window = tcp_window(endpoint);
        int      sequence_valid = receive_window ?
                                      (!seq_before(sequence, endpoint->rcv_nxt) && seq_before(sequence, endpoint->rcv_nxt + receive_window)) :
                                      sequence == endpoint->rcv_nxt;
        if (!sequence_valid
            && !(payload_length && seq_before(sequence, endpoint->rcv_nxt)
                 && seq_after(sequence + (uint32_t)payload_length, endpoint->rcv_nxt))) {
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
    }
    if (endpoint->state == TCP_FIN_WAIT_1 && !seq_before(endpoint->snd_una, endpoint->snd_nxt)) {
        endpoint->state           = TCP_FIN_WAIT_2;
        endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
    } else if (endpoint->state == TCP_CLOSING && !seq_before(endpoint->snd_una, endpoint->snd_nxt)) {
        endpoint->state           = TCP_TIME_WAIT;
        endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
    } else if (endpoint->state == TCP_LAST_ACK && !seq_before(endpoint->snd_una, endpoint->snd_nxt))
        endpoint->state = TCP_CLOSED;
    if (payload_length || (flags & TCP_FLAG_FIN)) {
        uint32_t       data_sequence   = sequence + !!(flags & TCP_FLAG_SYN);
        const uint8_t *payload         = tcp + header_length;
        size_t         accepted_length = payload_length;
        int            fin             = !!(flags & TCP_FLAG_FIN);
        if (seq_before(data_sequence, endpoint->rcv_nxt)) {
            uint32_t overlap = endpoint->rcv_nxt - data_sequence;
            if (overlap >= accepted_length) {
                if (overlap > accepted_length || !fin) fin = 0;
                accepted_length = 0;
            } else {
                payload += overlap;
                accepted_length -= overlap;
            }
            data_sequence = endpoint->rcv_nxt;
        }
        if (seq_after(data_sequence, endpoint->rcv_nxt)) {
            if (data_sequence - endpoint->rcv_nxt <= tcp_window(endpoint)) tcp_queue_ooo(endpoint, data_sequence, payload, accepted_length, fin);
        } else if (accepted_length <= TCP_RX_BUFFER_MAX - endpoint->rx_length) {
            if (accepted_length) {
                memcpy(endpoint->rx_data + endpoint->rx_length, payload, accepted_length);
                endpoint->rx_length += (uint16_t)accepted_length;
                endpoint->rcv_nxt += (uint32_t)accepted_length;
            }
            if (fin) tcp_received_fin(endpoint);
            tcp_drain_ooo(endpoint);
        }
        tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
    }
    uint32_t             ready6 = tcp_ready_locked(endpoint);
    tcp_event_callback_t cb6    = endpoint->event_callback;
    void                *ctx6   = endpoint->event_context;
    wait_queue_wake_all(&endpoint->wait);
    spin_unlock(&endpoint->lock);
    if (cb6) cb6(endpoint, ready6, ctx6);
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

int tcp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet)
{
    (void)device;
    if (!ip || !packet || packet->length < TCP_HEADER_LEN) goto bad;
    uint8_t *tcp           = packet->data;
    size_t   header_length = (size_t)(tcp[12] >> 4) * 4U;
    if (header_length < TCP_HEADER_LEN || header_length > packet->length
        || net_checksum_ipv4_pseudo(ip->source, ip->destination, IPV4_PROTO_TCP, tcp, packet->length) != 0)
        goto bad;
    uint16_t source_port      = net_read_be16(tcp);
    uint16_t destination_port = net_read_be16(tcp + 2);
    uint32_t sequence         = net_read_be32(tcp + 4);
    uint32_t acknowledgment   = net_read_be32(tcp + 8);
    uint8_t  flags            = tcp[13];
    uint16_t window           = net_read_be16(tcp + 14);
    size_t   payload_length   = packet->length - header_length;
    if (!source_port || !destination_port || (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) == (TCP_FLAG_SYN | TCP_FLAG_FIN)) goto bad;

    spin_lock(&tcp_table_lock);
    tcp_endpoint_t *listener;
    tcp_endpoint_t *endpoint = tcp_lookup_locked(ip, source_port, destination_port, &listener);
    if (!endpoint) {
        if (listener && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
            spin_lock(&listener->lock);
            int status = tcp_passive_open(listener, ip, source_port, sequence, tcp_parse_mss(tcp, header_length));
            spin_unlock(&listener->lock);
            spin_unlock(&tcp_table_lock);
            net_pbuf_free(packet);
            return status;
        }
        spin_unlock(&tcp_table_lock);
        int status = tcp_reset_reply(ip, source_port, destination_port, sequence, acknowledgment, flags, payload_length);
        net_pbuf_free(packet);
        return status ? status : -ECONNREFUSED;
    }
    spin_lock(&endpoint->lock);
    spin_unlock(&tcp_table_lock);

    if (flags & TCP_FLAG_RST) {
        int acceptable = endpoint->state == TCP_SYN_SENT ?
                             ((flags & TCP_FLAG_ACK) && acknowledgment == endpoint->snd_nxt) :
                             (!seq_before(sequence, endpoint->rcv_nxt) && !seq_after(sequence, endpoint->rcv_nxt + tcp_window(endpoint)));
        if (!acceptable) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->state              = TCP_CLOSED;
        endpoint->error              = ECONNRESET;
        tcp_event_callback_t cb_rst  = endpoint->event_callback;
        void                *ctx_rst = endpoint->event_context;
        wait_queue_wake_all(&endpoint->wait);
        spin_unlock(&endpoint->lock);
        if (cb_rst) cb_rst(endpoint, TCP_READY_ERROR | TCP_READY_READ | TCP_READY_HANGUP, ctx_rst);
        net_pbuf_free(packet);
        return -ECONNRESET;
    }
    uint64_t now = sched_ticks();
    if (endpoint->state == TCP_TIME_WAIT) {
        uint16_t receive_window = tcp_window(endpoint);
        int      acceptable     = receive_window ?
                                      (!seq_before(sequence, endpoint->rcv_nxt) && seq_before(sequence, endpoint->rcv_nxt + receive_window)) :
                                      sequence == endpoint->rcv_nxt;
        if (acceptable) {
            uint32_t segment_end = sequence + (uint32_t)payload_length + !!(flags & TCP_FLAG_FIN);
            if ((flags & TCP_FLAG_FIN) && segment_end == endpoint->rcv_nxt) endpoint->time_wait_until = now + TCP_TIME_WAIT_TICKS;
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
        }
        spin_unlock(&endpoint->lock);
        net_pbuf_free(packet);
        return 0;
    }
    if (endpoint->state != TCP_SYN_SENT && endpoint->state != TCP_SYN_RECEIVED && endpoint->state != TCP_LISTEN) {
        uint16_t receive_window = tcp_window(endpoint);
        int      sequence_valid = receive_window ?
                                      (!seq_before(sequence, endpoint->rcv_nxt) && seq_before(sequence, endpoint->rcv_nxt + receive_window)) :
                                      sequence == endpoint->rcv_nxt;
        if (!sequence_valid
            && !(payload_length && seq_before(sequence, endpoint->rcv_nxt)
                 && seq_after(sequence + (uint32_t)payload_length, endpoint->rcv_nxt))) {
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
    }
    endpoint->last_received         = now;
    endpoint->keepalive_probe_count = 0;
    endpoint->keepalive_deadline    = endpoint->keepalive_enabled ? now + endpoint->keepalive_idle : 0;
    if (flags & TCP_FLAG_ACK) {
        if (seq_before(acknowledgment, endpoint->snd_una) || seq_after(acknowledgment, endpoint->snd_nxt)) {
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EBADMSG;
        }
        uint16_t previous_window = endpoint->peer_window;
        if (seq_after(sequence, endpoint->snd_wl1) || (sequence == endpoint->snd_wl1 && !seq_before(acknowledgment, endpoint->snd_wl2))) {
            endpoint->peer_window = window;
            endpoint->snd_wl1     = sequence;
            endpoint->snd_wl2     = acknowledgment;
            if (!previous_window && window) {
                endpoint->persist_deadline = 0;
                endpoint->persist_interval = 0;
                if (endpoint->tx_head) {
                    tcp_tx_record_t *record = endpoint->tx_head;
                    if (!tcp_emit(endpoint, record->sequence, endpoint->rcv_nxt, record->flags, record->data, record->length, 0)) {
                        record->retransmitted = 1;
                        endpoint->retransmissions++;
                    }
                    record->deadline = now + endpoint->rto;
                }
                endpoint->persist_needed = 0;
            }
        }
        if (acknowledgment == endpoint->snd_una && endpoint->tx_head && endpoint->peer_window && window == previous_window && !payload_length
            && !(flags & (TCP_FLAG_SYN | TCP_FLAG_FIN))) {
            if (++endpoint->duplicate_acks == 3) {
                uint32_t flight    = endpoint->snd_nxt - endpoint->snd_una;
                endpoint->ssthresh = flight / 2;
                if (endpoint->ssthresh < 2U * endpoint->peer_mss) endpoint->ssthresh = 2U * endpoint->peer_mss;
                endpoint->cwnd          = endpoint->ssthresh + 3U * endpoint->peer_mss;
                endpoint->recover       = endpoint->snd_nxt;
                endpoint->fast_recovery = 1;
                tcp_tx_record_t *record = endpoint->tx_head;
                tcp_emit(endpoint, record->sequence, endpoint->rcv_nxt, record->flags, record->data, record->length, 0);
                record->retransmitted = 1;
                record->deadline      = now + endpoint->rto;
                endpoint->retransmissions++;
            } else if (endpoint->duplicate_acks > 3)
                endpoint->cwnd += endpoint->peer_mss;
        } else if (seq_after(acknowledgment, endpoint->snd_una))
            tcp_ack_records(endpoint, acknowledgment);
    }

    if (endpoint->state == TCP_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) != (TCP_FLAG_SYN | TCP_FLAG_ACK) || acknowledgment != endpoint->snd_nxt) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->rcv_nxt     = sequence + 1;
        endpoint->peer_mss    = tcp_parse_mss(tcp, header_length);
        endpoint->peer_window = window;
        endpoint->snd_wl1     = sequence;
        endpoint->snd_wl2     = acknowledgment;
        endpoint->state       = TCP_ESTABLISHED;
        tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
    } else if (endpoint->state == TCP_SYN_RECEIVED && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        tcp_tx_record_t *record = endpoint->tx_head;
        if (record) {
            tcp_emit(endpoint, record->sequence, endpoint->rcv_nxt, record->flags, NULL, 0, 0);
            record->retransmitted = 1;
            record->deadline      = sched_ticks() + endpoint->rto;
        }
        spin_unlock(&endpoint->lock);
        net_pbuf_free(packet);
        return 0;
    } else if (endpoint->state == TCP_SYN_RECEIVED) {
        if (!(flags & TCP_FLAG_ACK) || acknowledgment != endpoint->snd_nxt || sequence != endpoint->rcv_nxt) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->state        = TCP_ESTABLISHED;
        tcp_endpoint_t *parent = endpoint->parent;
        if (parent) {
            spin_lock(&parent->lock);
            if (parent->accept_count < parent->backlog) {
                for (unsigned n = 0; n < TCP_ACCEPT_MAX; n++) {
                    unsigned index = (parent->accept_tail + n) % TCP_ACCEPT_MAX;
                    if (!parent->accept_queue[index]) {
                        parent->accept_queue[index] = endpoint;
                        parent->accept_tail         = (uint8_t)((index + 1) % TCP_ACCEPT_MAX);
                        parent->accept_count++;
                        break;
                    }
                }
            }
            tcp_event_callback_t cb_parent  = parent->event_callback;
            void                *ctx_parent = parent->event_context;
            wait_queue_wake_all(&parent->wait);
            spin_unlock(&parent->lock);
            if (cb_parent) cb_parent(parent, TCP_READY_ACCEPT | TCP_READY_READ, ctx_parent);
        }
    } else if (endpoint->state != TCP_ESTABLISHED && endpoint->state != TCP_FIN_WAIT_1 && endpoint->state != TCP_FIN_WAIT_2
               && endpoint->state != TCP_CLOSE_WAIT && endpoint->state != TCP_CLOSING && endpoint->state != TCP_LAST_ACK) {
        plogk("tcp: segment on closed connection (local=%u remote=%u:%u).\n", (unsigned)endpoint->local_port, (unsigned)endpoint->remote_port,
              (unsigned)source_port);
        spin_unlock(&endpoint->lock);
        net_pbuf_free(packet);
        return -ENOTCONN;
    }

    if (endpoint->state == TCP_FIN_WAIT_1 && !seq_before(endpoint->snd_una, endpoint->snd_nxt)) {
        endpoint->state           = TCP_FIN_WAIT_2;
        endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
    } else if (endpoint->state == TCP_CLOSING && !seq_before(endpoint->snd_una, endpoint->snd_nxt)) {
        endpoint->state           = TCP_TIME_WAIT;
        endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
    } else if (endpoint->state == TCP_LAST_ACK && !seq_before(endpoint->snd_una, endpoint->snd_nxt))
        endpoint->state = TCP_CLOSED;

    if (payload_length || (flags & TCP_FLAG_FIN)) {
        uint32_t       data_sequence   = sequence + !!(flags & TCP_FLAG_SYN);
        const uint8_t *payload         = tcp + header_length;
        size_t         accepted_length = payload_length;
        int            fin             = !!(flags & TCP_FLAG_FIN);
        if (seq_before(data_sequence, endpoint->rcv_nxt)) {
            uint32_t overlap = endpoint->rcv_nxt - data_sequence;
            if (overlap >= accepted_length) {
                if (overlap > accepted_length || !fin) fin = 0;
                accepted_length = 0;
            } else {
                payload += overlap;
                accepted_length -= overlap;
            }
            data_sequence = endpoint->rcv_nxt;
        }
        if (seq_after(data_sequence, endpoint->rcv_nxt)) {
            if (data_sequence - endpoint->rcv_nxt <= tcp_window(endpoint)) tcp_queue_ooo(endpoint, data_sequence, payload, accepted_length, fin);
        } else if (accepted_length <= TCP_RX_BUFFER_MAX - endpoint->rx_length) {
            if (accepted_length) {
                memcpy(endpoint->rx_data + endpoint->rx_length, payload, accepted_length);
                endpoint->rx_length += (uint16_t)accepted_length;
                endpoint->rcv_nxt += (uint32_t)accepted_length;
            }
            if (fin) tcp_received_fin(endpoint);
            tcp_drain_ooo(endpoint);
        }
        tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
    }
    uint32_t             ready = tcp_ready_locked(endpoint);
    tcp_event_callback_t cb    = endpoint->event_callback;
    void                *ctx   = endpoint->event_context;
    wait_queue_wake_all(&endpoint->wait);
    spin_unlock(&endpoint->lock);
    if (cb) cb(endpoint, ready, ctx);
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

void tcp_timer(uint64_t now_ticks)
{
    tcp_endpoint_t      *deferred_ep[TCP_ENDPOINT_MAX];
    tcp_event_callback_t deferred_cb[TCP_ENDPOINT_MAX];
    void                *deferred_ctx[TCP_ENDPOINT_MAX];
    uint32_t             deferred_ready[TCP_ENDPOINT_MAX];
    int                  deferred_count = 0;

    spin_lock(&tcp_table_lock);
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *endpoint = tcp_table[i];
        if (!endpoint) continue;
        spin_lock(&endpoint->lock);
        if ((endpoint->state == TCP_TIME_WAIT || endpoint->state == TCP_FIN_WAIT_2) && now_ticks >= endpoint->time_wait_until)
            endpoint->state = TCP_CLOSED;
        int              failed = 0;
        tcp_tx_record_t *record = endpoint->tx_head;
        if (record && record->length && !endpoint->peer_window && endpoint->state != TCP_SYN_SENT && endpoint->state != TCP_SYN_RECEIVED) {
            if (!endpoint->persist_deadline) {
                endpoint->persist_interval = endpoint->rto > TCP_PERSIST_MIN ? endpoint->rto : TCP_PERSIST_MIN;
                endpoint->persist_deadline = now_ticks + endpoint->persist_interval;
            } else if (now_ticks >= endpoint->persist_deadline) {
                tcp_emit(endpoint, endpoint->snd_nxt - 1U, endpoint->rcv_nxt, TCP_FLAG_ACK, record->data, 1, 0);
                endpoint->persist_probes_sent++;
                endpoint->persist_interval = endpoint->persist_interval > TCP_RTO_MAX / 2U ? TCP_RTO_MAX : endpoint->persist_interval * 2U;
                endpoint->persist_deadline = now_ticks + endpoint->persist_interval;
            }
        } else if (!endpoint->peer_window && endpoint->persist_needed && endpoint->persist_deadline && now_ticks >= endpoint->persist_deadline) {
            tcp_emit(endpoint, endpoint->snd_nxt - 1U, endpoint->rcv_nxt, TCP_FLAG_ACK, &endpoint->persist_byte, 1, 0);
            endpoint->persist_probes_sent++;
            endpoint->persist_interval = endpoint->persist_interval > TCP_RTO_MAX / 2U ? TCP_RTO_MAX : endpoint->persist_interval * 2U;
            endpoint->persist_deadline = now_ticks + endpoint->persist_interval;
        } else if (record && now_ticks >= record->deadline) {
            uint8_t retry_limit
                = (endpoint->state == TCP_SYN_SENT || endpoint->state == TCP_SYN_RECEIVED) ? endpoint->syn_retries : endpoint->data_retries;
            if (record->retries >= retry_limit) {
                plogk("tcp: retransmission timed out (local=%u remote=%u state=%u).\n", (unsigned)endpoint->local_port,
                      (unsigned)endpoint->remote_port, (unsigned)endpoint->state);
                tcp_fail_locked(endpoint, ETIMEDOUT);
                failed = 1;
            } else {
                int status = tcp_emit(endpoint, record->sequence, endpoint->rcv_nxt, record->flags, record->data, record->length, 0);
                record->retries++;
                if (!status) {
                    record->retransmitted = 1;
                    endpoint->retransmissions++;
                    uint32_t flight    = endpoint->snd_nxt - endpoint->snd_una;
                    endpoint->ssthresh = flight / 2;
                    if (endpoint->ssthresh < 2U * endpoint->peer_mss) endpoint->ssthresh = 2U * endpoint->peer_mss;
                    endpoint->cwnd          = endpoint->peer_mss;
                    endpoint->fast_recovery = 0;
                }
                uint32_t shift   = record->retries > 5 ? 5 : record->retries;
                uint32_t backoff = endpoint->rto << shift;
                record->deadline = now_ticks + (backoff > TCP_RTO_MAX ? TCP_RTO_MAX : backoff);
            }
        } else if (endpoint->keepalive_enabled && endpoint->state == TCP_ESTABLISHED && !record && now_ticks >= endpoint->keepalive_deadline) {
            if (endpoint->keepalive_probe_count >= endpoint->keepalive_count) {
                plogk("tcp: keepalive timed out (local=%u remote=%u).\n", (unsigned)endpoint->local_port, (unsigned)endpoint->remote_port);
                tcp_fail_locked(endpoint, ETIMEDOUT);
                failed = 1;
            } else {
                tcp_emit(endpoint, endpoint->snd_nxt - 1U, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
                endpoint->keepalive_probe_count++;
                endpoint->keepalive_probes_sent++;
                endpoint->keepalive_deadline = now_ticks + endpoint->keepalive_interval;
            }
        }
        if (failed) {
            wait_queue_wake_all(&endpoint->wait);
            if (!endpoint->orphaned) {
                deferred_ep[deferred_count]    = endpoint;
                deferred_cb[deferred_count]    = endpoint->event_callback;
                deferred_ctx[deferred_count]   = endpoint->event_context;
                deferred_ready[deferred_count] = tcp_ready_locked(endpoint);
                deferred_count++;
                spin_unlock(&endpoint->lock);
            } else {
                spin_unlock(&endpoint->lock);
                tcp_table[i] = NULL;
                tcp_records_free(endpoint->tx_head);
                tcp_ooo_free(endpoint->ooo_head);
                free(endpoint->rx_data);
                free(endpoint);
            }
        } else {
            int destroy = endpoint->orphaned && endpoint->state == TCP_CLOSED;
            spin_unlock(&endpoint->lock);
            if (destroy) {
                tcp_table[i] = NULL;
                tcp_records_free(endpoint->tx_head);
                tcp_ooo_free(endpoint->ooo_head);
                free(endpoint->rx_data);
                free(endpoint);
            }
        }
    }
    spin_unlock(&tcp_table_lock);
    for (int i = 0; i < deferred_count; i++)
        if (deferred_cb[i]) deferred_cb[i](deferred_ep[i], deferred_ready[i], deferred_ctx[i]);
}

uint32_t tcp_readiness(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return TCP_READY_ERROR | TCP_READY_HANGUP;
    spin_lock(&endpoint->lock);
    uint32_t ready = tcp_ready_locked(endpoint);
    spin_unlock(&endpoint->lock);
    return ready;
}

int tcp_get_info(tcp_endpoint_t *endpoint, tcp_endpoint_info_t *info)
{
    if (!endpoint || !info) return -EINVAL;
    spin_lock(&endpoint->lock);
    info->family              = endpoint->family;
    info->local_address       = endpoint->local_address;
    info->remote_address      = endpoint->remote_address;
    info->local_address6      = endpoint->local_address6;
    info->remote_address6     = endpoint->remote_address6;
    info->local_port          = endpoint->local_port;
    info->remote_port         = endpoint->remote_port;
    info->state               = endpoint->state;
    info->receive_queued      = endpoint->rx_length;
    info->send_unacknowledged = endpoint->snd_nxt - endpoint->snd_una;
    info->congestion_window   = endpoint->cwnd;
    info->receive_window      = tcp_window(endpoint);
    info->send_window         = endpoint->peer_window;
    info->peer_mss            = endpoint->peer_mss;
    info->retransmit_timeout  = endpoint->rto;
    info->retransmissions     = endpoint->retransmissions;
    info->keepalive_probes    = endpoint->keepalive_probes_sent;
    info->persist_probes      = endpoint->persist_probes_sent;
    info->duplicate_acks      = endpoint->duplicate_acks;
    info->queued_segments     = tcp_tx_count(endpoint);
    info->last_received_ticks = endpoint->last_received;
    uint64_t next_timer       = endpoint->time_wait_until;
    if (endpoint->tx_head && !endpoint->persist_deadline && (!next_timer || endpoint->tx_head->deadline < next_timer))
        next_timer = endpoint->tx_head->deadline;
    if (endpoint->persist_deadline && (!next_timer || endpoint->persist_deadline < next_timer)) next_timer = endpoint->persist_deadline;
    if (endpoint->keepalive_deadline && (!next_timer || endpoint->keepalive_deadline < next_timer)) next_timer = endpoint->keepalive_deadline;
    info->next_timer_ticks  = next_timer;
    info->keepalive_enabled = endpoint->keepalive_enabled;
    spin_unlock(&endpoint->lock);
    return 0;
}

void tcp_set_event_callback(tcp_endpoint_t *endpoint, tcp_event_callback_t callback, void *context)
{
    if (!endpoint) return;
    spin_lock(&endpoint->lock);
    endpoint->event_callback = callback;
    endpoint->event_context  = callback ? context : NULL;
    uint32_t ready           = tcp_ready_locked(endpoint);
    spin_unlock(&endpoint->lock);
    if (callback) callback(endpoint, ready, context);
}

void tcp_set_v6only(tcp_endpoint_t *endpoint, int enabled)
{
    if (!endpoint) return;
    spin_lock(&endpoint->lock);
    endpoint->v6only = enabled != 0;
    spin_unlock(&endpoint->lock);
}

wait_queue_t *tcp_wait_queue(tcp_endpoint_t *endpoint)
{
    return endpoint ? &endpoint->wait : NULL;
}
