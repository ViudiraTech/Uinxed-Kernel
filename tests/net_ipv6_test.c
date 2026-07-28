#include <kernel/errno.h>
#include <net/byteorder.h>
#include <net/ethernet.h>
#include <net/icmpv6.h>
#include <net/ipv6.h>
#include <net/ndp.h>
#include <net/pbuf.h>
#include <net/tcp.h>
#include <proc/task.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sync/spin_lock.h>

#define MAX_FRAMES 16U
#define MAX_FRAME_SIZE 1600U

typedef struct mock_link {
    unsigned calls;
    size_t lengths[MAX_FRAMES];
    uint8_t frames[MAX_FRAMES][MAX_FRAME_SIZE];
} mock_link_t;

static int failures;
static uint64_t mock_ticks;
static unsigned transport_calls;
static uint8_t transport_data[32];
static size_t transport_length;

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

static const ipv6_address_t local_address = {
    .bytes = {0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 0, 0x02, 0, 0, 0xff, 0xfe, 0, 0, 1},
};
static const ipv6_address_t remote_address = {
    .bytes = {0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 0, 0x02, 0, 0, 0xff, 0xfe, 0, 0, 2},
};
static const ipv6_address_t remote_link_local = {
    .bytes = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0, 0, 0xff, 0xfe, 0, 0, 2},
};

static uint32_t checksum_add(uint32_t sum, const uint8_t *bytes, size_t length)
{
    while (length >= 2) {
        sum += ((uint32_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length) sum += (uint32_t)bytes[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t fixture_ipv6_checksum(const ipv6_address_t *source, const ipv6_address_t *destination, uint8_t protocol,
                                      const void *data, size_t length)
{
    uint8_t tail[8] = {0};
    uint32_t sum = checksum_add(0, source->bytes, 16);
    sum = checksum_add(sum, destination->bytes, 16);
    net_write_be32(tail, (uint32_t)length);
    tail[7] = protocol;
    sum = checksum_add(sum, tail, sizeof(tail));
    return checksum_finish(checksum_add(sum, data, length));
}

static void make_ipv6(uint8_t *packet, size_t payload_length, uint8_t next_header,
                      const ipv6_address_t *source, const ipv6_address_t *destination)
{
    memset(packet, 0, IPV6_HEADER_LEN + payload_length);
    packet[0] = 0x60;
    net_write_be16(packet + 4, (uint16_t)payload_length);
    packet[6] = next_header;
    packet[7] = 64;
    memcpy(packet + 8, source->bytes, 16);
    memcpy(packet + 24, destination->bytes, 16);
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
    static const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
    memset(link, 0, sizeof(*link));
    CHECK(netdev_init(device, "ip6test0", &mock_ops, link) == 0, "IPv6 mock netdev init");
    memcpy(device->address, mac, 6);
    memcpy(device->ipv6_address, local_address.bytes, 16);
    ipv6_address_t link_local;
    ipv6_link_local_from_mac(&link_local, mac);
    memcpy(device->ipv6_link_local, link_local.bytes, 16);
    CHECK(netdev_register(device) == 0 && netdev_set_up(device, 1) == 0, "IPv6 mock netdev registration");
    memset(link, 0, sizeof(*link));
}

static void teardown_device(net_device_t *device)
{
    ndp_device_removed(device);
    ipv6_device_removed(device);
    CHECK(netdev_unregister(device) == 0, "IPv6 mock netdev unregister");
}

static void test_ipv6_header_extensions_and_checksum(void)
{
    uint8_t packet[59];
    net_ipv6_packet_t parsed;
    static const uint8_t odd[] = {0x12, 0x34, 0, 53, 0, 11, 0, 0, 'd', 'n', 's'};

    make_ipv6(packet, 19, IPV6_NEXT_HOP_BY_HOP, &remote_address, &local_address);
    packet[40] = IPV6_NEXT_UDP;
    packet[41] = 0;
    memcpy(packet + 48, odd, sizeof(odd));
    CHECK(net_ipv6_parse(packet, 39, &parsed) < 0, "truncated IPv6 fixed header accepted");
    CHECK(net_ipv6_parse(packet, sizeof(packet), &parsed) == 0, "valid hop-by-hop chain rejected");
    CHECK(parsed.protocol == IPV6_NEXT_UDP && parsed.transport_offset == 48 && parsed.payload == packet + 48
              && parsed.payload_len == sizeof(odd) && parsed.total_len == sizeof(packet),
          "IPv6 extension transport boundary is wrong");
    CHECK(ipv6_address_equal(&parsed.source, &remote_address) && ipv6_address_equal(&parsed.destination, &local_address),
          "IPv6 addresses changed during parse");
    CHECK(net_checksum_ipv6_pseudo(&remote_address, &local_address, IPV6_NEXT_UDP, odd, sizeof(odd))
              == fixture_ipv6_checksum(&remote_address, &local_address, IPV6_NEXT_UDP, odd, sizeof(odd)),
          "IPv6 odd-length pseudo-header checksum vector");

    packet[0] = 0x40;
    CHECK(net_ipv6_parse(packet, sizeof(packet), &parsed) < 0, "non-IPv6 version accepted");
    packet[0] = 0x60;
    net_write_be16(packet + 4, 20);
    CHECK(net_ipv6_parse(packet, sizeof(packet), &parsed) < 0, "payload length beyond receive span accepted");
    net_write_be16(packet + 4, 19);
    packet[41] = 1;
    CHECK(net_ipv6_parse(packet, sizeof(packet), &parsed) < 0, "truncated extension length accepted");

    uint8_t fragment[64];
    make_ipv6(fragment, 24, IPV6_NEXT_FRAGMENT, &remote_address, &local_address);
    fragment[40] = IPV6_NEXT_UDP;
    net_write_be16(fragment + 42, 0x0011);
    net_write_be32(fragment + 44, 0x12345678U);
    memcpy(fragment + 48, "0123456789abcdef", 16);
    CHECK(net_ipv6_parse(fragment, sizeof(fragment), &parsed) == 0 && parsed.has_fragment && parsed.more_fragments
              && parsed.fragment_offset == 16 && parsed.fragment_id == 0x12345678U && parsed.protocol == IPV6_NEXT_UDP,
          "IPv6 fragment header fields decoded incorrectly");
}

static void finish_icmpv6(uint8_t *data, size_t length, const ipv6_address_t *source, const ipv6_address_t *destination)
{
    data[2] = data[3] = 0;
    net_write_be16(data + 2, fixture_ipv6_checksum(source, destination, IPV6_NEXT_ICMP, data, length));
}

static net_pbuf_t *icmp_packet(const void *data, size_t length)
{
    return net_pbuf_from(data, length, NET_PBUF_HEADROOM);
}

static void test_ndp_option_bounds_ns_na_and_slaac(void)
{
    static const uint8_t remote_mac[6] = {0x02, 0, 0, 0, 0, 2};
    static const ipv6_address_t all_nodes = {.bytes = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
    net_device_t device;
    mock_link_t link;
    ipv6_info_t info = {.source = remote_link_local, .destination = all_nodes, .protocol = IPV6_NEXT_ICMP, .hop_limit = 255};
    uint8_t message[56] = {0};

    setup_device(&device, &link);
    message[0] = ICMPV6_ROUTER_ADVERT;
    message[6] = 0x07;
    message[7] = 0x08;
    message[16] = 1;
    message[17] = 1;
    memcpy(message + 18, remote_mac, 6);
    message[24] = 3;
    message[25] = 4;
    message[26] = 64;
    message[27] = 0xc0;
    net_write_be32(message + 28, 7200);
    net_write_be32(message + 32, 3600);
    memcpy(message + 40, local_address.bytes, 8);
    finish_icmpv6(message, sizeof(message), &remote_link_local, &all_nodes);
    net_pbuf_t *packet = icmp_packet(message, sizeof(message));
    CHECK(packet && ndp_input(&device, &info, packet) == 0, "valid RA/SLAAC options rejected");
    CHECK(device.ipv6_prefix_length == 64 && !memcmp(device.ipv6_address, local_address.bytes, 8)
              && !memcmp(device.ipv6_default_router, remote_link_local.bytes, 16),
          "RA prefix/router was not installed with SLAAC bounds");

    message[17] = 0;
    finish_icmpv6(message, sizeof(message), &remote_link_local, &all_nodes);
    packet = icmp_packet(message, sizeof(message));
    CHECK(packet && ndp_input(&device, &info, packet) == -EBADMSG, "zero-length NDP option accepted");
    message[17] = 1;
    packet = icmp_packet(message, 19);
    CHECK(packet && ndp_input(&device, &info, packet) == -EBADMSG, "truncated NDP option accepted");

    ipv6_address_t link_local;
    memcpy(link_local.bytes, device.ipv6_link_local, 16);
    memset(message, 0, 32);
    message[0] = ICMPV6_NEIGHBOR_SOLICIT;
    memcpy(message + 8, link_local.bytes, 16);
    message[24] = 1;
    message[25] = 1;
    memcpy(message + 26, remote_mac, 6);
    info.destination = link_local;
    finish_icmpv6(message, 32, &remote_link_local, &link_local);
    unsigned before = link.calls;
    packet = icmp_packet(message, 32);
    CHECK(packet && ndp_input(&device, &info, packet) == 0 && link.calls == before + 1,
          "neighbor solicitation did not produce an advertisement");
    teardown_device(&device);
}

static void test_neighbor_pending_fifo(void)
{
    static const uint8_t remote_mac[6] = {0x02, 0, 0, 0, 0, 2};
    net_device_t device;
    mock_link_t link;
    setup_device(&device, &link);
    net_pbuf_t *first = net_pbuf_from("first", 5, NET_PBUF_HEADROOM);
    net_pbuf_t *second = net_pbuf_from("second", 6, NET_PBUF_HEADROOM);
    CHECK(first && second, "NDP pending packet allocation");
    CHECK(ndp_resolve(&device, &remote_address, first) == -EINPROGRESS && link.calls == 1,
          "first neighbor miss did not queue and solicit");
    CHECK(ndp_resolve(&device, &remote_address, second) == -EINPROGRESS && link.calls == 1,
          "concurrent neighbor miss was not coalesced");
    CHECK(first->refs == 1 && second->refs == 1, "NDP consumed caller-owned pending pbuf");
    ndp_learn(&device, &remote_address, remote_mac, mock_ticks);
    CHECK(link.calls == 3 && link.lengths[1] == ETH_HEADER_LEN + 5 && link.lengths[2] == ETH_HEADER_LEN + 6
              && !memcmp(link.frames[1] + ETH_HEADER_LEN, "first", 5)
              && !memcmp(link.frames[2] + ETH_HEADER_LEN, "second", 6),
          "neighbor learn did not flush pending packets in FIFO order");
    net_pbuf_free(first);
    net_pbuf_free(second);
    teardown_device(&device);
}

static int capture_transport(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet)
{
    (void)device;
    (void)ip;
    transport_calls++;
    transport_length = packet->length < sizeof(transport_data) ? packet->length : sizeof(transport_data);
    memcpy(transport_data, packet->data, transport_length);
    net_pbuf_free(packet);
    return 0;
}

static net_pbuf_t *make_fragment(uint32_t id, uint16_t offset, int more, const void *payload, size_t length)
{
    net_pbuf_t *packet = net_pbuf_alloc(IPV6_HEADER_LEN + 8 + length, NET_PBUF_HEADROOM);
    if (!packet) return NULL;
    make_ipv6(packet->data, 8 + length, IPV6_NEXT_FRAGMENT, &remote_address, &local_address);
    packet->data[40] = 253;
    net_write_be16(packet->data + 42, (uint16_t)((offset & 0xfff8U) | !!more));
    net_write_be32(packet->data + 44, id);
    memcpy(packet->data + 48, payload, length);
    return packet;
}

static void test_ipv6_fragment_overlap_rejection(void)
{
    net_device_t device;
    mock_link_t link;
    setup_device(&device, &link);
    CHECK(ipv6_set_transport_handler(253, capture_transport) == 0, "test IPv6 transport registration");
    transport_calls = 0;
    net_pbuf_t *packet = make_fragment(0xabcdef01U, 0, 1, "abcdefghijklmnop", 16);
    CHECK(packet && ipv6_input(&device, packet) == -EINPROGRESS, "first IPv6 fragment was not held");
    packet = make_fragment(0xabcdef01U, 8, 0, "overlap!", 8);
    CHECK(packet && ipv6_input(&device, packet) == -EBADMSG, "overlapping IPv6 fragment was not rejected");
    CHECK(transport_calls == 0, "overlapping fragment set reached transport");

    packet = make_fragment(0xabcdef02U, 8, 0, "ijklmnop", 8);
    CHECK(packet && ipv6_input(&device, packet) == -EINPROGRESS, "out-of-order final IPv6 fragment was not held");
    packet = make_fragment(0xabcdef02U, 0, 1, "abcdefgh", 8);
    CHECK(packet && ipv6_input(&device, packet) == 0 && transport_calls == 1 && transport_length == 16
              && !memcmp(transport_data, "abcdefghijklmnop", 16),
          "non-overlapping IPv6 fragments did not reassemble after overlap rejection");
    CHECK(ipv6_set_transport_handler(253, NULL) == 0, "test IPv6 transport detach");
    teardown_device(&device);
}

static void test_udpv6_dns_tuple_and_mandatory_checksum(void)
{
    static const uint8_t dns[] = {0x12, 0x34, 0x01, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    uint8_t udp[8 + sizeof(dns)] = {0};
    net_write_be16(udp, 49152);
    net_write_be16(udp + 2, 53);
    net_write_be16(udp + 4, sizeof(udp));
    memcpy(udp + 8, dns, sizeof(dns));
    uint16_t checksum = net_checksum_ipv6_pseudo(&local_address, &remote_address, IPV6_NEXT_UDP, udp, sizeof(udp));
    net_write_be16(udp + 6, checksum ? checksum : UINT16_MAX);
    CHECK(net_read_be16(udp) == 49152 && net_read_be16(udp + 2) == 53 && net_read_be16(udp + 4) == sizeof(udp),
          "UDPv6 DNS tuple encoding changed");
    CHECK(net_read_be16(udp + 6) != 0
              && net_checksum_ipv6_pseudo(&local_address, &remote_address, IPV6_NEXT_UDP, udp, sizeof(udp)) == 0,
          "UDPv6 mandatory checksum did not validate");
    udp[sizeof(udp) - 1] ^= 1;
    CHECK(net_checksum_ipv6_pseudo(&local_address, &remote_address, IPV6_NEXT_UDP, udp, sizeof(udp)) != 0,
          "corrupt UDPv6 DNS payload retained a valid checksum");
}

static void test_tcpv6_segment_when_parser_available(void)
{
    struct in6_addr source, destination;
    memcpy(source.s6_addr, local_address.bytes, 16);
    memcpy(destination.s6_addr, remote_address.bytes, 16);
    uint8_t tcp[20] = {0};
    net_write_be16(tcp, 49153);
    net_write_be16(tcp + 2, 443);
    net_write_be32(tcp + 4, 0x10203040U);
    net_write_be32(tcp + 8, 0x50607080U);
    tcp[12] = 5U << 4;
    tcp[13] = 0x12;
    net_write_be16(tcp + 14, 4096);
    uint16_t checksum = net_checksum_ipv6_pseudo(&local_address, &remote_address, IPV6_NEXT_TCP, tcp, sizeof(tcp));
    net_write_be16(tcp + 16, checksum ? checksum : UINT16_MAX);
    net_tcp_segment_t parsed;
    CHECK(net_tcp_parse6(tcp, sizeof(tcp), &source, &destination, &parsed) == 0,
          "valid TCPv6 SYN-ACK vector rejected");
    CHECK(parsed.source_port == 49153 && parsed.destination_port == 443 && parsed.sequence == 0x10203040U
              && parsed.acknowledgment == 0x50607080U && parsed.flags == 0x12,
          "TCPv6 tuple or handshake fields decoded incorrectly");
    tcp[4] ^= 1;
    CHECK(net_tcp_parse6(tcp, sizeof(tcp), &source, &destination, &parsed) == -EBADMSG,
          "bad TCPv6 checksum accepted");
}

int main(void)
{
    test_ipv6_header_extensions_and_checksum();
    test_ndp_option_bounds_ns_na_and_slaac();
    test_neighbor_pending_fifo();
    test_ipv6_fragment_overlap_rejection();
    test_udpv6_dns_tuple_and_mandatory_checksum();
    test_tcpv6_segment_when_parser_available();
    if (failures) {
        printf("%d IPv6 host test(s) failed\n", failures);
        return 1;
    }
    printf("PASS IPv6 parsing/checksum, NDP/SLAAC, pending queue, reassembly, and UDPv6 vectors\n");
    return 0;
}
