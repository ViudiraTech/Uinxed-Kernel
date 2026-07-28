#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/endian.h>
#include <net/tcp.h>
#include <proc/sched.h>

#define TCP_HEADER_LEN       20U
#define TCP_FLAG_FIN         0x01U
#define TCP_FLAG_SYN         0x02U
#define TCP_FLAG_RST         0x04U
#define TCP_FLAG_PSH         0x08U
#define TCP_FLAG_ACK         0x10U
#define TCP_EPHEMERAL_FIRST  49152U
#define TCP_DEFAULT_MSS      536U
#define TCP_RTO_TICKS        100U
#define TCP_RETRY_MAX        6U
#define TCP_TIME_WAIT_TICKS  6000U

typedef struct tcp_tx_record {
        struct tcp_tx_record *next;
        uint32_t              sequence;
        uint32_t              end_sequence;
        uint16_t              length;
        uint8_t               flags;
        uint8_t               retries;
        uint64_t              deadline;
        uint8_t               data[];
} tcp_tx_record_t;

struct tcp_endpoint {
        uint32_t         local_address;
        uint32_t         remote_address;
        uint16_t         local_port;
        uint16_t         remote_port;
        tcp_state_t      state;
        uint32_t         snd_una;
        uint32_t         snd_nxt;
        uint32_t         rcv_nxt;
        uint16_t         peer_window;
        uint16_t         rx_length;
        uint8_t         *rx_data;
        int              error;
        uint8_t          bound;
        uint8_t          backlog;
        uint8_t          accept_head;
        uint8_t          accept_tail;
        uint8_t          accept_count;
        uint64_t         time_wait_until;
        struct tcp_endpoint *parent;
        struct tcp_endpoint *accept_queue[TCP_ACCEPT_MAX];
        tcp_tx_record_t *tx_head;
        wait_queue_t     wait;
        spinlock_t       lock;
};

static tcp_endpoint_t *tcp_table[TCP_ENDPOINT_MAX];
static spinlock_t tcp_table_lock;
static spinlock_t tcp_iss_lock;
static uint16_t tcp_ephemeral = TCP_EPHEMERAL_FIRST;
static uint32_t tcp_iss_counter;

static int seq_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

int net_tcp_seq_before(uint32_t a, uint32_t b) { return seq_before(a, b); }
int net_tcp_seq_after(uint32_t a, uint32_t b) { return seq_after(a, b); }

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
    const uint8_t *bytes = data;
    size_t header_length = (size_t)(bytes[12] >> 4) * 4U;
    if (header_length < TCP_HEADER_LEN || header_length > length
        || net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_TCP, bytes, length) != 0) return -EBADMSG;
    segment->source_port = net_read_be16(bytes);
    segment->destination_port = net_read_be16(bytes + 2);
    segment->sequence = net_read_be32(bytes + 4);
    segment->acknowledgment = net_read_be32(bytes + 8);
    segment->header_len = (uint8_t)header_length;
    segment->flags = bytes[13];
    segment->payload = bytes + header_length;
    segment->payload_len = length - header_length;
    return 0;
}

static uint16_t tcp_window(const tcp_endpoint_t *endpoint) { return (uint16_t)(TCP_RX_BUFFER_MAX - endpoint->rx_length); }

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
        if (ep && ep != ignore && ep->bound && ep->local_port == port && (!ep->local_address || !address || ep->local_address == address)) return 1;
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
    if (!endpoint) return NULL;
    endpoint->rx_data = malloc(TCP_RX_BUFFER_MAX);
    if (!endpoint->rx_data) {
        free(endpoint);
        return NULL;
    }
    endpoint->state = TCP_CLOSED;
    endpoint->peer_window = UINT16_MAX;
    wait_queue_init(&endpoint->wait);
    if (tcp_insert_locked(endpoint)) {
        free(endpoint->rx_data);
        free(endpoint);
        return NULL;
    }
    return endpoint;
}

tcp_endpoint_t *tcp_open(void)
{
    spin_lock(&tcp_table_lock);
    tcp_endpoint_t *endpoint = tcp_alloc_locked();
    spin_unlock(&tcp_table_lock);
    return endpoint;
}

static void tcp_records_free(tcp_tx_record_t *record)
{
    while (record) {
        tcp_tx_record_t *next = record->next;
        free(record);
        record = next;
    }
}

void tcp_close(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return;
    spin_lock(&tcp_table_lock);
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) if (tcp_table[i] == endpoint) tcp_table[i] = NULL;
    if (endpoint->parent) {
        tcp_endpoint_t *parent = endpoint->parent;
        spin_lock(&parent->lock);
        for (unsigned i = 0; i < TCP_ACCEPT_MAX; i++) if (parent->accept_queue[i] == endpoint) parent->accept_queue[i] = NULL;
        spin_unlock(&parent->lock);
    }
    spin_lock(&endpoint->lock);
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *child = tcp_table[i];
        if (!child || child->parent != endpoint) continue;
        tcp_table[i] = NULL;
        tcp_records_free(child->tx_head);
        free(child->rx_data);
        free(child);
    }
    memset(endpoint->accept_queue, 0, sizeof(endpoint->accept_queue));
    tcp_tx_record_t *records = endpoint->tx_head;
    endpoint->tx_head = NULL;
    spin_unlock(&endpoint->lock);
    spin_unlock(&tcp_table_lock);
    wait_queue_wake_all(&endpoint->wait);
    tcp_records_free(records);
    free(endpoint->rx_data);
    free(endpoint);
}

int tcp_bind(tcp_endpoint_t *endpoint, uint32_t address, uint16_t port)
{
    if (!endpoint || !port) return -EINVAL;
    spin_lock(&tcp_table_lock);
    if (endpoint->bound || tcp_port_used_locked(address, port, endpoint)) {
        spin_unlock(&tcp_table_lock);
        return endpoint->bound ? -EINVAL : -EADDRINUSE;
    }
    endpoint->local_address = address;
    endpoint->local_port = port;
    endpoint->bound = 1;
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
            endpoint->local_port = port;
            endpoint->bound = 1;
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
    endpoint->state = TCP_LISTEN;
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
            accepted = endpoint->accept_queue[index];
            endpoint->accept_queue[index] = NULL;
            endpoint->accept_head = (uint8_t)((index + 1) % TCP_ACCEPT_MAX);
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
    if (length > UINT16_MAX - TCP_HEADER_LEN) return -EMSGSIZE;
    net_device_t *device;
    uint32_t next_hop;
    int status = ipv4_route(endpoint->remote_address, &device, &next_hop);
    if (status) return status;
    net_pbuf_t *packet = net_pbuf_alloc(TCP_HEADER_LEN + length, NET_PBUF_HEADROOM);
    if (!packet) {
        netdev_put(device);
        return -ENOMEM;
    }
    uint8_t *tcp = packet->data;
    memset(tcp, 0, TCP_HEADER_LEN);
    net_write_be16(tcp, endpoint->local_port);
    net_write_be16(tcp + 2, endpoint->remote_port);
    net_write_be32(tcp + 4, sequence);
    net_write_be32(tcp + 8, acknowledgment);
    tcp[12] = 5U << 4;
    tcp[13] = flags;
    net_write_be16(tcp + 14, tcp_window(endpoint));
    if (length) memcpy(tcp + TCP_HEADER_LEN, data, length);
    uint16_t checksum = net_checksum_ipv4_pseudo(endpoint->local_address, endpoint->remote_address, IPV4_PROTO_TCP, tcp, packet->length);
    net_write_be16(tcp + 16, checksum);
    tcp_tx_record_t *record = NULL;
    uint32_t sequence_length = (uint32_t)length + !!(flags & TCP_FLAG_SYN) + !!(flags & TCP_FLAG_FIN);
    if (track && sequence_length) {
        record = malloc(sizeof(*record) + length);
        if (!record) {
            net_pbuf_free(packet);
            netdev_put(device);
            return -ENOMEM;
        }
        record->next = NULL;
        record->sequence = sequence;
        record->end_sequence = sequence + sequence_length;
        record->length = (uint16_t)length;
        record->flags = flags;
        record->retries = 0;
        record->deadline = sched_ticks() + TCP_RTO_TICKS;
        if (length) memcpy(record->data, data, length);
    }
    status = ipv4_output(device, endpoint->local_address, endpoint->remote_address, IPV4_PROTO_TCP, 64, packet);
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
    uint32_t next_hop;
    int status = ipv4_route(address, &device, &next_hop);
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
    endpoint->remote_port = port;
    endpoint->snd_una = tcp_new_iss();
    endpoint->snd_nxt = endpoint->snd_una + 1;
    endpoint->state = TCP_SYN_SENT;
    status = tcp_emit(endpoint, endpoint->snd_una, 0, TCP_FLAG_SYN, NULL, 0, 1);
    if (status) endpoint->state = TCP_CLOSED;
    spin_unlock(&endpoint->lock);
    return status ? status : -EINPROGRESS;
}

int tcp_send(tcp_endpoint_t *endpoint, const void *data, size_t length)
{
    if (!endpoint || (!data && length)) return -EINVAL;
    if (!length) return 0;
    spin_lock(&endpoint->lock);
    if (endpoint->state != TCP_ESTABLISHED && endpoint->state != TCP_CLOSE_WAIT) {
        spin_unlock(&endpoint->lock);
        return -ENOTCONN;
    }
    size_t sent = 0;
    while (sent < length) {
        unsigned records = 0;
        for (tcp_tx_record_t *r = endpoint->tx_head; r; r = r->next) records++;
        if (records >= TCP_TX_SEGMENT_MAX || endpoint->peer_window <= endpoint->snd_nxt - endpoint->snd_una) break;
        size_t allowed = endpoint->peer_window - (endpoint->snd_nxt - endpoint->snd_una);
        size_t chunk = length - sent;
        if (chunk > TCP_DEFAULT_MSS) chunk = TCP_DEFAULT_MSS;
        if (chunk > allowed) chunk = allowed;
        if (!chunk) break;
        int status = tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH, (const uint8_t *)data + sent, chunk, 1);
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
    }
    spin_unlock(&endpoint->lock);
    return (int)copied;
}

int tcp_shutdown(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    spin_lock(&endpoint->lock);
    tcp_state_t next;
    if (endpoint->state == TCP_ESTABLISHED) next = TCP_FIN_WAIT_1;
    else if (endpoint->state == TCP_CLOSE_WAIT) next = TCP_LAST_ACK;
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

tcp_state_t tcp_get_state(const tcp_endpoint_t *endpoint) { return endpoint ? endpoint->state : TCP_CLOSED; }

int tcp_get_error(tcp_endpoint_t *endpoint)
{
    if (!endpoint) return EINVAL;
    spin_lock(&endpoint->lock);
    int error = endpoint->error;
    endpoint->error = 0;
    spin_unlock(&endpoint->lock);
    return error;
}

static void tcp_ack_records(tcp_endpoint_t *endpoint, uint32_t acknowledgment)
{
    while (endpoint->tx_head && !seq_before(acknowledgment, endpoint->tx_head->end_sequence)) {
        tcp_tx_record_t *record = endpoint->tx_head;
        endpoint->tx_head = record->next;
        free(record);
    }
    endpoint->snd_una = acknowledgment;
}

static tcp_endpoint_t *tcp_lookup_locked(const ipv4_info_t *ip, uint16_t source_port, uint16_t destination_port, tcp_endpoint_t **listener)
{
    *listener = NULL;
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *ep = tcp_table[i];
        if (!ep || !ep->bound || ep->local_port != destination_port || (ep->local_address && ep->local_address != ip->destination)) continue;
        if (ep->state == TCP_LISTEN) *listener = ep;
        else if (ep->remote_address == ip->source && ep->remote_port == source_port) return ep;
    }
    return NULL;
}

static int tcp_reset_reply(const ipv4_info_t *ip, uint16_t source_port, uint16_t destination_port, uint32_t sequence, uint32_t acknowledgment,
                           uint8_t flags, size_t payload_length)
{
    if (flags & TCP_FLAG_RST) return 0;
    tcp_endpoint_t temporary;
    memset(&temporary, 0, sizeof(temporary));
    temporary.local_address = ip->destination;
    temporary.remote_address = ip->source;
    temporary.local_port = destination_port;
    temporary.remote_port = source_port;
    if (flags & TCP_FLAG_ACK) return tcp_emit(&temporary, acknowledgment, 0, TCP_FLAG_RST, NULL, 0, 0);
    uint32_t ack = sequence + (uint32_t)payload_length + !!(flags & TCP_FLAG_SYN) + !!(flags & TCP_FLAG_FIN);
    return tcp_emit(&temporary, 0, ack, TCP_FLAG_RST | TCP_FLAG_ACK, NULL, 0, 0);
}

static int tcp_passive_open(tcp_endpoint_t *listener, const ipv4_info_t *ip, uint16_t source_port, uint32_t sequence)
{
    if (listener->accept_count >= listener->backlog) return -ENOBUFS;
    tcp_endpoint_t *child = tcp_alloc_locked();
    if (!child) return -ENOBUFS;
    child->bound = 1;
    child->local_address = ip->destination;
    child->local_port = listener->local_port;
    child->remote_address = ip->source;
    child->remote_port = source_port;
    child->rcv_nxt = sequence + 1;
    child->snd_una = tcp_new_iss();
    child->snd_nxt = child->snd_una + 1;
    child->state = TCP_SYN_RECEIVED;
    child->parent = listener;
    spin_lock(&child->lock);
    int status = tcp_emit(child, child->snd_una, child->rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0, 1);
    spin_unlock(&child->lock);
    if (status) {
        for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) if (tcp_table[i] == child) tcp_table[i] = NULL;
        free(child->rx_data);
        free(child);
    }
    return status;
}

int tcp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet)
{
    (void)device;
    if (!ip || !packet || packet->length < TCP_HEADER_LEN) goto bad;
    uint8_t *tcp = packet->data;
    size_t header_length = (size_t)(tcp[12] >> 4) * 4U;
    if (header_length < TCP_HEADER_LEN || header_length > packet->length
        || net_checksum_ipv4_pseudo(ip->source, ip->destination, IPV4_PROTO_TCP, tcp, packet->length) != 0) goto bad;
    uint16_t source_port = net_read_be16(tcp);
    uint16_t destination_port = net_read_be16(tcp + 2);
    uint32_t sequence = net_read_be32(tcp + 4);
    uint32_t acknowledgment = net_read_be32(tcp + 8);
    uint8_t flags = tcp[13];
    uint16_t window = net_read_be16(tcp + 14);
    size_t payload_length = packet->length - header_length;
    if (!source_port || !destination_port || (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) == (TCP_FLAG_SYN | TCP_FLAG_FIN)) goto bad;

    spin_lock(&tcp_table_lock);
    tcp_endpoint_t *listener;
    tcp_endpoint_t *endpoint = tcp_lookup_locked(ip, source_port, destination_port, &listener);
    if (!endpoint) {
        if (listener && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
            spin_lock(&listener->lock);
            int status = tcp_passive_open(listener, ip, source_port, sequence);
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
        endpoint->state = TCP_CLOSED;
        endpoint->error = ECONNRESET;
        spin_unlock(&endpoint->lock);
        wait_queue_wake_all(&endpoint->wait);
        net_pbuf_free(packet);
        return -ECONNRESET;
    }
    endpoint->peer_window = window;
    if (flags & TCP_FLAG_ACK) {
        if (seq_before(acknowledgment, endpoint->snd_una) || seq_after(acknowledgment, endpoint->snd_nxt)) {
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EBADMSG;
        }
        tcp_ack_records(endpoint, acknowledgment);
    }

    if (endpoint->state == TCP_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) != (TCP_FLAG_SYN | TCP_FLAG_ACK) || acknowledgment != endpoint->snd_nxt) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->rcv_nxt = sequence + 1;
        endpoint->state = TCP_ESTABLISHED;
        tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
    } else if (endpoint->state == TCP_SYN_RECEIVED) {
        if (!(flags & TCP_FLAG_ACK) || acknowledgment != endpoint->snd_nxt || sequence != endpoint->rcv_nxt) {
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        endpoint->state = TCP_ESTABLISHED;
        tcp_endpoint_t *parent = endpoint->parent;
        if (parent) {
            spin_lock(&parent->lock);
            if (parent->accept_count < parent->backlog) {
                for (unsigned n = 0; n < TCP_ACCEPT_MAX; n++) {
                    unsigned index = (parent->accept_tail + n) % TCP_ACCEPT_MAX;
                    if (!parent->accept_queue[index]) {
                        parent->accept_queue[index] = endpoint;
                        parent->accept_tail = (uint8_t)((index + 1) % TCP_ACCEPT_MAX);
                        parent->accept_count++;
                        break;
                    }
                }
            }
            spin_unlock(&parent->lock);
            wait_queue_wake_one(&parent->wait);
        }
    } else if (endpoint->state != TCP_ESTABLISHED && endpoint->state != TCP_FIN_WAIT_1 && endpoint->state != TCP_FIN_WAIT_2
               && endpoint->state != TCP_CLOSE_WAIT && endpoint->state != TCP_CLOSING && endpoint->state != TCP_LAST_ACK) {
        spin_unlock(&endpoint->lock);
        net_pbuf_free(packet);
        return -ENOTCONN;
    }

    if (endpoint->state == TCP_FIN_WAIT_1 && !seq_before(endpoint->snd_una, endpoint->snd_nxt)) endpoint->state = TCP_FIN_WAIT_2;
    else if (endpoint->state == TCP_CLOSING && !seq_before(endpoint->snd_una, endpoint->snd_nxt)) {
        endpoint->state = TCP_TIME_WAIT;
        endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
    } else if (endpoint->state == TCP_LAST_ACK && !seq_before(endpoint->snd_una, endpoint->snd_nxt)) endpoint->state = TCP_CLOSED;

    if (payload_length || (flags & TCP_FLAG_FIN)) {
        if (sequence != endpoint->rcv_nxt) {
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -EAGAIN;
        }
        if (payload_length > TCP_RX_BUFFER_MAX - endpoint->rx_length) {
            tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
            spin_unlock(&endpoint->lock);
            net_pbuf_free(packet);
            return -ENOBUFS;
        }
        if (payload_length) {
            memcpy(endpoint->rx_data + endpoint->rx_length, tcp + header_length, payload_length);
            endpoint->rx_length += (uint16_t)payload_length;
            endpoint->rcv_nxt += (uint32_t)payload_length;
        }
        if (flags & TCP_FLAG_FIN) {
            endpoint->rcv_nxt++;
            if (endpoint->state == TCP_ESTABLISHED) endpoint->state = TCP_CLOSE_WAIT;
            else if (endpoint->state == TCP_FIN_WAIT_1) endpoint->state = TCP_CLOSING;
            else if (endpoint->state == TCP_FIN_WAIT_2) {
                endpoint->state = TCP_TIME_WAIT;
                endpoint->time_wait_until = sched_ticks() + TCP_TIME_WAIT_TICKS;
            }
        }
        tcp_emit(endpoint, endpoint->snd_nxt, endpoint->rcv_nxt, TCP_FLAG_ACK, NULL, 0, 0);
    }
    spin_unlock(&endpoint->lock);
    wait_queue_wake_all(&endpoint->wait);
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

void tcp_timer(uint64_t now_ticks)
{
    spin_lock(&tcp_table_lock);
    for (unsigned i = 0; i < TCP_ENDPOINT_MAX; i++) {
        tcp_endpoint_t *endpoint = tcp_table[i];
        if (!endpoint) continue;
        spin_lock(&endpoint->lock);
        if (endpoint->state == TCP_TIME_WAIT && now_ticks >= endpoint->time_wait_until) endpoint->state = TCP_CLOSED;
        tcp_tx_record_t *record = endpoint->tx_head;
        if (record && now_ticks >= record->deadline) {
            if (record->retries >= TCP_RETRY_MAX) {
                endpoint->state = TCP_CLOSED;
                endpoint->error = ETIMEDOUT;
                tcp_records_free(endpoint->tx_head);
                endpoint->tx_head = NULL;
                wait_queue_wake_all(&endpoint->wait);
            } else {
                tcp_emit(endpoint, record->sequence, endpoint->rcv_nxt, record->flags, record->data, record->length, 0);
                record->retries++;
                record->deadline = now_ticks + (TCP_RTO_TICKS << (record->retries > 5 ? 5 : record->retries));
            }
        }
        spin_unlock(&endpoint->lock);
    }
    spin_unlock(&tcp_table_lock);
}
