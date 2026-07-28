#include <kernel/errno.h>
#include <net/dhcp.h>
#include <net/arp.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <net/ethernet.h>
#include <net/ipv4.h>
#include <net/netdev.h>
#include <net/pbuf.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <proc/task.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sync/spin_lock.h>

#define LOCAL_IP  0xc0000201U
#define REMOTE_IP 0xc0000202U
#define OTHER_IP  0xc0000203U
#define MAX_FRAMES 32U
#define MAX_FRAME_SIZE 1600U

typedef struct mock_link {
    unsigned calls;
    size_t lengths[MAX_FRAMES];
    uint8_t frames[MAX_FRAMES][MAX_FRAME_SIZE];
} mock_link_t;

static int failures;
static uint64_t mock_ticks;

#define CHECK(condition, message)                                    \
    do {                                                             \
        if (!(condition)) {                                          \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, message); \
            failures++;                                              \
            return;                                                  \
        }                                                            \
    } while (0)

void spin_lock(spinlock_t *lock) { (void)lock; }
void spin_unlock(spinlock_t *lock) { (void)lock; }
uint64_t sched_ticks(void) { return mock_ticks; }
void wait_queue_init(wait_queue_t *queue) { memset(queue, 0, sizeof(*queue)); }
task_t *wait_queue_wake_one(wait_queue_t *queue)
{
    (void)queue;
    return NULL;
}
uint64_t wait_queue_wake_all(wait_queue_t *queue)
{
    (void)queue;
    return 0;
}

static int mock_xmit(net_device_t *device, net_pbuf_t *packet)
{
    mock_link_t *link = device->driver_data;
    unsigned index = link->calls++;
    if (index < MAX_FRAMES) {
        size_t length = packet->length < MAX_FRAME_SIZE ? packet->length : MAX_FRAME_SIZE;
        link->lengths[index] = length;
        memcpy(link->frames[index], packet->data, length);
    }
    return 0;
}

static const netdev_ops_t mock_ops = {.xmit = mock_xmit};

static void setup_device(net_device_t *device, mock_link_t *link)
{
    static const uint8_t local_mac[6] = {0x02, 0, 0, 0, 0, 1};

    memset(link, 0, sizeof(*link));
    CHECK(netdev_init(device, "path0", &mock_ops, link) == 0, "mock netdev init");
    memcpy(device->address, local_mac, sizeof(local_mac));
    CHECK(netdev_register(device) == 0, "mock netdev register");
    CHECK(netdev_configure_ipv4(device, LOCAL_IP, 0xffffff00U, 0) == 0, "mock IPv4 configuration");
    CHECK(netdev_set_up(device, 1) == 0, "mock netdev up");
    memset(link, 0, sizeof(*link));
}

static void teardown_device(net_device_t *device)
{
    arp_device_removed(device);
    CHECK(netdev_unregister(device) == 0, "mock netdev unregister");
}

static net_pbuf_t *make_udp_packet(uint32_t source, uint32_t destination, uint16_t source_port, uint16_t destination_port,
                                   const void *payload, size_t payload_length)
{
    net_pbuf_t *packet = net_pbuf_alloc(8 + payload_length, NET_PBUF_HEADROOM);
    if (!packet) return NULL;
    net_write_be16(packet->data, source_port);
    net_write_be16(packet->data + 2, destination_port);
    net_write_be16(packet->data + 4, (uint16_t)packet->length);
    net_write_be16(packet->data + 6, 0);
    memcpy(packet->data + 8, payload, payload_length);
    uint16_t checksum = net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_UDP, packet->data, packet->length);
    net_write_be16(packet->data + 6, checksum ? checksum : UINT16_MAX);
    return packet;
}

static net_pbuf_t *make_tcp_packet(uint32_t source, uint32_t destination, uint16_t source_port, uint16_t destination_port,
                                   uint32_t sequence, uint32_t acknowledgment, uint8_t flags, const void *payload,
                                   size_t payload_length)
{
    net_pbuf_t *packet = net_pbuf_alloc(20 + payload_length, NET_PBUF_HEADROOM);
    if (!packet) return NULL;
    memset(packet->data, 0, 20);
    net_write_be16(packet->data, source_port);
    net_write_be16(packet->data + 2, destination_port);
    net_write_be32(packet->data + 4, sequence);
    net_write_be32(packet->data + 8, acknowledgment);
    packet->data[12] = 5U << 4;
    packet->data[13] = flags;
    net_write_be16(packet->data + 14, UINT16_MAX);
    if (payload_length) memcpy(packet->data + 20, payload, payload_length);
    net_write_be16(packet->data + 16,
                   net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_TCP, packet->data, packet->length));
    return packet;
}

static int parse_frame(const mock_link_t *link, unsigned index, net_ipv4_packet_t *ip, net_tcp_segment_t *tcp,
                       net_udp_datagram_t *udp)
{
    net_ethernet_frame_t ethernet;
    if (index >= link->calls || index >= MAX_FRAMES || net_ethernet_parse(link->frames[index], link->lengths[index], &ethernet)
        || ethernet.ether_type != ETH_TYPE_IPV4 || net_ipv4_parse(ethernet.payload, ethernet.payload_len, ip))
        return -1;
    if (tcp) return net_tcp_parse(ip->payload, ip->payload_len, ip->source, ip->destination, tcp);
    if (udp) return net_udp_parse(ip->payload, ip->payload_len, ip->source, ip->destination, udp);
    return 0;
}

static void test_dns_udp_exchange_semantics(void)
{
    static const uint8_t remote_mac[6] = {0x02, 0, 0, 0, 0, 2};
    static const uint8_t query[] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x07, 'e',  'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00,
                                    0x00, 0x01, 0x00, 0x01};
    static const uint8_t response[] = {0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01};
    net_device_t device;
    mock_link_t link;
    net_ipv4_packet_t ip;
    net_udp_datagram_t wire;
    udp_datagram_t info;
    uint8_t buffer[32] = {0};

    setup_device(&device, &link);
    arp_learn(&device, REMOTE_IP, remote_mac, mock_ticks);
    udp_endpoint_t *endpoint = udp_open();
    CHECK(endpoint != NULL, "UDP endpoint allocation");
    CHECK(udp_connect(endpoint, REMOTE_IP, 53) == 0, "connected DNS UDP endpoint");
    CHECK(udp_send(endpoint, query, sizeof(query), 0, 0) == (int)sizeof(query), "DNS query send length");
    CHECK(link.calls == 1 && parse_frame(&link, 0, &ip, NULL, &wire) == 0, "DNS query wire packet");
    CHECK(ip.source == LOCAL_IP && ip.destination == REMOTE_IP && wire.destination_port == 53
              && wire.source_port == udp_local_port(endpoint) && wire.payload_len == sizeof(query)
              && !memcmp(wire.payload, query, sizeof(query)),
          "DNS query tuple or transaction bytes changed");

    ipv4_info_t inbound = {.source = REMOTE_IP, .destination = LOCAL_IP, .protocol = IPV4_PROTO_UDP};
    net_pbuf_t *packet = make_udp_packet(REMOTE_IP, LOCAL_IP, 53, udp_local_port(endpoint), response, sizeof(response));
    CHECK(packet && udp_input(&device, &inbound, packet) == 0, "DNS response delivery");
    CHECK(udp_receive(endpoint, buffer, 4, &info, 1) == 4, "DNS response peek truncation");
    CHECK(info.source_address == REMOTE_IP && info.source_port == 53 && info.length == sizeof(response),
          "DNS response source/full-length metadata");
    CHECK(udp_receive(endpoint, buffer, sizeof(buffer), &info, 0) == (int)sizeof(response)
              && !memcmp(buffer, response, sizeof(response)),
          "peek consumed or changed DNS datagram");
    CHECK(udp_receive(endpoint, buffer, sizeof(buffer), NULL, 0) == -EAGAIN, "empty UDP queue not nonblocking");
    udp_close(endpoint);
    teardown_device(&device);
}

static void test_dhcp_malformed_options(void)
{
    static const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
    uint8_t packet[256] = {0};
    dhcp_reply_t reply;

    packet[0] = 2;
    packet[1] = 1;
    packet[2] = 6;
    net_write_be32(packet + 4, 0x12345678U);
    net_write_be32(packet + 16, LOCAL_IP);
    memcpy(packet + 28, mac, sizeof(mac));
    net_write_be32(packet + 236, 0x63825363U);
    packet[240] = 53;
    packet[241] = 1;
    packet[242] = 5;
    packet[243] = 255;
    CHECK(dhcp_parse_reply(packet, 244, 0x12345678U, mac, &reply) == 0 && reply.message_type == 5,
          "valid minimal DHCP ACK rejected");
    CHECK(dhcp_parse_reply(packet, 239, 0x12345678U, mac, &reply) == -EBADMSG, "truncated DHCP fixed header accepted");
    packet[243] = 1;
    CHECK(dhcp_parse_reply(packet, 244, 0x12345678U, mac, &reply) == -EBADMSG, "missing DHCP end option accepted");
    packet[240] = 6;
    packet[241] = 5;
    CHECK(dhcp_parse_reply(packet, 244, 0x12345678U, mac, &reply) == -EBADMSG, "truncated DHCP option value accepted");
    packet[240] = 53;
    packet[241] = 0;
    CHECK(dhcp_parse_reply(packet, 244, 0x12345678U, mac, &reply) == -EBADMSG, "bad DHCP message-type length accepted");
}

static void test_tcp_active_handshake_retransmission_reassembly_and_fin(void)
{
    static const uint8_t remote_mac[6] = {0x02, 0, 0, 0, 0, 2};
    net_device_t device;
    mock_link_t link;
    net_ipv4_packet_t ip;
    net_tcp_segment_t syn, retransmit;
    ipv4_info_t inbound = {.source = REMOTE_IP, .destination = LOCAL_IP, .protocol = IPV4_PROTO_TCP};
    uint8_t data[8] = {0};

    mock_ticks = 10;
    setup_device(&device, &link);
    arp_learn(&device, REMOTE_IP, remote_mac, mock_ticks);
    tcp_endpoint_t *endpoint = tcp_open();
    CHECK(endpoint != NULL, "TCP endpoint allocation");
    CHECK(tcp_connect(endpoint, REMOTE_IP, 443) == -EINPROGRESS && tcp_get_state(endpoint) == TCP_SYN_SENT,
          "active connect did not enter nonblocking SYN_SENT");
    CHECK(tcp_get_error(endpoint) == 0, "in-progress connect exposed a socket error");
    CHECK(tcp_readiness(endpoint) == 0, "pending connect reported writable before completion");
    CHECK(link.calls == 1 && parse_frame(&link, 0, &ip, &syn, NULL) == 0 && syn.flags == 0x02,
          "active connect did not emit SYN");

    tcp_timer(109);
    CHECK(link.calls == 1, "SYN retransmitted before RTO");
    tcp_timer(110);
    CHECK(link.calls == 2 && parse_frame(&link, 1, &ip, &retransmit, NULL) == 0 && retransmit.flags == 0x02
              && retransmit.sequence == syn.sequence,
          "RTO did not retransmit the original SYN");

    net_pbuf_t *packet = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 7000, syn.sequence + 1, 0x12, NULL, 0);
    CHECK(packet && tcp_input(&device, &inbound, packet) == 0 && tcp_get_state(endpoint) == TCP_ESTABLISHED,
          "valid SYN-ACK did not establish active connection");
    CHECK(tcp_get_error(endpoint) == 0, "successful connect SO_ERROR model is not zero");
    CHECK((tcp_readiness(endpoint) & TCP_READY_WRITE) != 0, "completed connect did not become writable");
    CHECK(link.calls == 3, "handshake completion did not emit final ACK");

    packet = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 7004, syn.sequence + 1, 0x18, "def", 3);
    CHECK(packet && tcp_input(&device, &inbound, packet) == 0, "out-of-order payload was not queued");
    CHECK(tcp_receive(endpoint, data, sizeof(data)) == -EAGAIN, "out-of-order payload became readable early");
    packet = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 7001, syn.sequence + 1, 0x18, "abc", 3);
    CHECK(packet && tcp_input(&device, &inbound, packet) == 0, "in-order payload rejected");
    CHECK(tcp_receive(endpoint, data, sizeof(data)) == 6 && !memcmp(data, "abcdef", 6), "TCP stream was not reassembled in order");

    packet = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 7007, syn.sequence + 1, 0x11, NULL, 0);
    CHECK(packet && tcp_input(&device, &inbound, packet) == 0 && tcp_get_state(endpoint) == TCP_CLOSE_WAIT,
          "peer FIN did not enter CLOSE_WAIT");
    CHECK(tcp_receive(endpoint, data, sizeof(data)) == 0, "FIN did not produce EOF after queued data");
    CHECK(tcp_shutdown(endpoint) == 0 && tcp_get_state(endpoint) == TCP_LAST_ACK, "close after peer FIN did not emit local FIN");
    tcp_close(endpoint);
    teardown_device(&device);
}

static void test_tcp_so_error_reset_and_clear(void)
{
    static const uint8_t remote_mac[6] = {0x02, 0, 0, 0, 0, 2};
    net_device_t device;
    mock_link_t link;
    net_ipv4_packet_t ip;
    net_tcp_segment_t syn;
    ipv4_info_t inbound = {.source = REMOTE_IP, .destination = LOCAL_IP, .protocol = IPV4_PROTO_TCP};

    setup_device(&device, &link);
    arp_learn(&device, REMOTE_IP, remote_mac, mock_ticks);
    tcp_endpoint_t *endpoint = tcp_open();
    CHECK(endpoint && tcp_connect(endpoint, REMOTE_IP, 443) == -EINPROGRESS, "reset test connect");
    CHECK(parse_frame(&link, 0, &ip, &syn, NULL) == 0, "reset test SYN parse");
    net_pbuf_t *packet = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 0, syn.sequence + 1, 0x14, NULL, 0);
    CHECK(packet && tcp_input(&device, &inbound, packet) == -ECONNRESET && tcp_get_state(endpoint) == TCP_CLOSED,
          "RST did not fail pending connect");
    CHECK((tcp_readiness(endpoint) & (TCP_READY_ERROR | TCP_READY_HANGUP)) == (TCP_READY_ERROR | TCP_READY_HANGUP),
          "failed connect readiness omitted error/hangup");
    CHECK(tcp_get_error(endpoint) == ECONNRESET && tcp_get_error(endpoint) == 0, "SO_ERROR was not read-and-cleared");
    tcp_close(endpoint);
    teardown_device(&device);
}

static void establish_tcp(net_device_t *device, mock_link_t *link, tcp_endpoint_t **endpoint, net_tcp_segment_t *syn)
{
    ipv4_info_t inbound = {.source = REMOTE_IP, .destination = LOCAL_IP, .protocol = IPV4_PROTO_TCP};
    *endpoint = tcp_open();
    CHECK(*endpoint && tcp_connect(*endpoint, REMOTE_IP, 443) == -EINPROGRESS, "timer test connect");
    net_ipv4_packet_t ip;
    CHECK(parse_frame(link, 0, &ip, syn, NULL) == 0, "timer test SYN parse");
    net_pbuf_t *packet = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn->source_port, 7000, syn->sequence + 1, 0x12, NULL, 0);
    CHECK(packet && tcp_input(device, &inbound, packet) == 0 && tcp_get_state(*endpoint) == TCP_ESTABLISHED,
          "timer test handshake");
}

static void test_tcp_loss_rto_and_long_idle(void)
{
    static const uint8_t remote_mac[6] = {0x02, 0, 0, 0, 0, 2};
    net_device_t device;
    mock_link_t link;
    net_tcp_segment_t syn;
    tcp_endpoint_info_t before, after;

    mock_ticks = 1000;
    setup_device(&device, &link);
    arp_learn(&device, REMOTE_IP, remote_mac, mock_ticks);
    tcp_endpoint_t *endpoint;
    establish_tcp(&device, &link, &endpoint, &syn);
    unsigned baseline = link.calls;
    CHECK(tcp_send(endpoint, "loss", 4) == 4 && tcp_get_info(endpoint, &before) == 0, "loss test send");
    CHECK(before.send_unacknowledged == 4 && before.queued_segments == 1, "loss was not tracked for retransmission");
    tcp_timer(before.next_timer_ticks - 1);
    CHECK(link.calls == baseline + 1, "data retransmitted before its RTO");
    tcp_timer(before.next_timer_ticks);
    CHECK(link.calls == baseline + 2 && tcp_get_info(endpoint, &after) == 0 && after.retransmissions == 1,
          "RTO did not retransmit exactly once");
    CHECK(after.congestion_window <= before.congestion_window, "RTO loss did not reduce congestion window");

    ipv4_info_t inbound = {.source = REMOTE_IP, .destination = LOCAL_IP, .protocol = IPV4_PROTO_TCP};
    net_pbuf_t *ack = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 7001, syn.sequence + 5, 0x10, NULL, 0);
    mock_ticks = after.next_timer_ticks;
    CHECK(ack && tcp_input(&device, &inbound, ack) == 0, "loss test acknowledgment");
    CHECK(tcp_get_info(endpoint, &after) == 0 && after.send_unacknowledged == 0, "ACK did not clear retransmission queue");
    baseline = link.calls;
    tcp_timer(UINT64_C(1) << 40);
    CHECK(tcp_get_state(endpoint) == TCP_ESTABLISHED && link.calls == baseline,
          "healthy connection changed state or transmitted during long idle without keepalive");
    tcp_close(endpoint);
    teardown_device(&device);
}

static void test_tcp_zero_window_persist_and_keepalive(void)
{
    static const uint8_t remote_mac[6] = {0x02, 0, 0, 0, 0, 2};
    net_device_t device;
    mock_link_t link;
    net_tcp_segment_t syn;
    tcp_endpoint_info_t info;
    ipv4_info_t inbound = {.source = REMOTE_IP, .destination = LOCAL_IP, .protocol = IPV4_PROTO_TCP};

    mock_ticks = 2000;
    setup_device(&device, &link);
    arp_learn(&device, REMOTE_IP, remote_mac, mock_ticks);
    tcp_endpoint_t *endpoint;
    establish_tcp(&device, &link, &endpoint, &syn);
    net_pbuf_t *window_zero = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 7001, syn.sequence + 1, 0x10, NULL, 0);
    CHECK(window_zero != NULL, "zero-window packet allocation");
    net_write_be16(window_zero->data + 14, 0);
    net_write_be16(window_zero->data + 16, 0);
    net_write_be16(window_zero->data + 16,
                   net_checksum_ipv4_pseudo(REMOTE_IP, LOCAL_IP, IPV4_PROTO_TCP, window_zero->data, window_zero->length));
    CHECK(tcp_input(&device, &inbound, window_zero) == 0, "zero-window update rejected");
    CHECK(tcp_get_info(endpoint, &info) == 0 && info.send_window == 0, "zero-window advertisement was not recorded");
    CHECK(tcp_send(endpoint, "queued", 6) == 1, "zero window did not retain exactly one byte for persist");
    CHECK(tcp_get_info(endpoint, &info) == 0 && info.send_unacknowledged == 0,
          "persist byte was incorrectly counted as transmitted");
    tcp_timer(info.next_timer_ticks);
    CHECK(tcp_get_info(endpoint, &info) == 0 && info.persist_probes == 1,
          "persist timer did not account for one zero-window probe");

    CHECK(tcp_set_option(endpoint, TCP_OPTION_KEEPIDLE_TICKS, 10) == 0
              && tcp_set_option(endpoint, TCP_OPTION_KEEPINTVL_TICKS, 5) == 0
              && tcp_set_option(endpoint, TCP_OPTION_KEEPCNT, 2) == 0
              && tcp_set_option(endpoint, TCP_OPTION_KEEPALIVE, 1) == 0,
          "keepalive option setup");
    window_zero = make_tcp_packet(REMOTE_IP, LOCAL_IP, 443, syn.source_port, 7001, syn.sequence + 1, 0x10, NULL, 0);
    CHECK(window_zero != NULL, "window reopen packet allocation");
    CHECK(tcp_input(&device, &inbound, window_zero) == 0 && tcp_get_info(endpoint, &info) == 0,
          "window reopen update rejected");
    unsigned baseline = link.calls;
    tcp_timer(info.next_timer_ticks);
    CHECK(link.calls == baseline + 1 && tcp_get_info(endpoint, &info) == 0 && info.keepalive_probes == 1,
          "keepalive idle timer did not emit first probe");
    tcp_timer(info.next_timer_ticks);
    CHECK(link.calls == baseline + 2 && tcp_get_info(endpoint, &info) == 0 && info.keepalive_probes == 2,
          "keepalive interval did not emit second probe");
    tcp_timer(info.next_timer_ticks);
    CHECK(tcp_get_state(endpoint) == TCP_CLOSED && tcp_get_error(endpoint) == ETIMEDOUT,
          "keepalive probe exhaustion did not time out connection");
    tcp_close(endpoint);
    teardown_device(&device);
}

static void test_arp_concurrent_pending_and_packet_ownership(void)
{
    static const uint8_t resolved_mac[6] = {0x02, 0, 0, 0, 0, 3};
    net_device_t device;
    mock_link_t link;
    net_pbuf_t *first, *second;

    setup_device(&device, &link);
    first = net_pbuf_from("first", 5, NET_PBUF_HEADROOM);
    second = net_pbuf_from("second", 6, NET_PBUF_HEADROOM);
    CHECK(first && second, "ARP pending pbuf allocation");
    CHECK(arp_resolve(&device, OTHER_IP, first) == -EINPROGRESS && link.calls == 1, "first ARP miss did not queue/request");
    CHECK(arp_resolve(&device, OTHER_IP, second) == -EINPROGRESS && link.calls == 1,
          "concurrent ARP miss was not coalesced behind one request");
    CHECK(first->refs == 1 && second->refs == 1, "ARP resolution consumed caller-owned pbuf");
    arp_learn(&device, OTHER_IP, resolved_mac, mock_ticks);
    CHECK(link.calls == 3 && link.lengths[1] == ETH_HEADER_LEN + 5 && link.lengths[2] == ETH_HEADER_LEN + 6
              && !memcmp(link.frames[1] + ETH_HEADER_LEN, "first", 5)
              && !memcmp(link.frames[2] + ETH_HEADER_LEN, "second", 6),
          "ARP learn did not flush concurrent pending packets in FIFO order");
    net_pbuf_free(first);
    net_pbuf_free(second);
    teardown_device(&device);
}

typedef struct release_count {
    unsigned calls;
} release_count_t;

static void count_release(void *context, void *data)
{
    (void)data;
    ((release_count_t *)context)->calls++;
}

static void make_ipv4_fragment(uint8_t storage[28], uint16_t fragment, const void *payload)
{
    memset(storage, 0, 28);
    storage[0] = 0x45;
    net_write_be16(storage + 2, 28);
    net_write_be16(storage + 4, 0x4242);
    net_write_be16(storage + 6, fragment);
    storage[8] = 64;
    storage[9] = IPV4_PROTO_UDP;
    net_write_be32(storage + 12, REMOTE_IP);
    net_write_be32(storage + 16, LOCAL_IP);
    memcpy(storage + 20, payload, 8);
    net_write_be16(storage + 10, net_checksum(storage, 20));
}

static void test_ipv4_fragment_reassembly_and_rx_ownership(void)
{
    net_device_t device;
    mock_link_t link;
    release_count_t release = {0};
    uint8_t first_storage[28], last_storage[28], udp_header[8], data[8] = {0};
    net_pbuf_t first = {.storage = first_storage, .data = first_storage, .length = sizeof(first_storage), .capacity = sizeof(first_storage),
                        .refs = 1, .release = count_release, .release_context = &release, .external = 1};
    net_pbuf_t last = {.storage = last_storage, .data = last_storage, .length = sizeof(last_storage), .capacity = sizeof(last_storage),
                       .refs = 1, .release = count_release, .release_context = &release, .external = 1};
    udp_datagram_t info;

    setup_device(&device, &link);
    udp_endpoint_t *endpoint = udp_open();
    CHECK(endpoint && udp_bind(endpoint, LOCAL_IP, 9000) == 0, "fragment destination UDP bind");
    net_write_be16(udp_header, 53);
    net_write_be16(udp_header + 2, 9000);
    net_write_be16(udp_header + 4, 16);
    net_write_be16(udp_header + 6, 0);
    make_ipv4_fragment(first_storage, IPV4_FLAG_MF, udp_header);
    make_ipv4_fragment(last_storage, 1, "fragment");
    CHECK(ipv4_input(&device, &last) == -EINPROGRESS, "last IPv4 fragment was not held for its missing prefix");
    CHECK(release.calls == 1, "held fragment input was not consumed exactly once");
    CHECK(ipv4_input(&device, &first) == 0, "out-of-order IPv4 fragments did not complete reassembly");
    CHECK(release.calls == 2, "completing fragment input was not consumed exactly once");
    CHECK(udp_receive(endpoint, data, sizeof(data), &info, 0) == 8 && !memcmp(data, "fragment", 8)
              && info.source_address == REMOTE_IP && info.source_port == 53,
          "reassembled IPv4 payload did not preserve the UDP datagram");
    udp_close(endpoint);
    teardown_device(&device);
}

int main(void)
{
    test_dns_udp_exchange_semantics();
    test_dhcp_malformed_options();
    test_tcp_active_handshake_retransmission_reassembly_and_fin();
    test_tcp_so_error_reset_and_clear();
    test_tcp_loss_rto_and_long_idle();
    test_tcp_zero_window_persist_and_keepalive();
    test_arp_concurrent_pending_and_packet_ownership();
    test_ipv4_fragment_reassembly_and_rx_ownership();

    if (failures) {
        printf("%d net path test(s) failed\n", failures);
        return 1;
    }
    printf("PASS DNS/DHCP, TCP lifecycle, ARP pending, IPv4 reassembly, and packet ownership semantics\n");
    return 0;
}
