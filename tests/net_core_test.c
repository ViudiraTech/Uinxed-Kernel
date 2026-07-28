#if defined(__has_include)
#if !__has_include(<net/byteorder.h>) || !__has_include(<net/checksum.h>) || !__has_include(<net/ethernet.h>) || \
    !__has_include(<net/arp.h>) || !__has_include(<net/ipv4.h>) || !__has_include(<net/udp.h>) || \
    !__has_include(<net/tcp.h>) || !__has_include(<net/packet.h>) || !__has_include(<net/netdev.h>)
#define NET_TEST_API_MISSING 1
#endif
#else
#define NET_TEST_API_MISSING 1
#endif

#ifdef NET_TEST_API_MISSING
#error "net core host tests require include/net/{byteorder,checksum,ethernet,arp,ipv4,udp,tcp,packet,netdev}.h; see tests/net_CONTRACT.md"
#else

#include <kernel/errno.h>
#include <net/arp.h>
#include <net/byteorder.h>
#include <net/checksum.h>
#include <net/ethernet.h>
#include <net/ipv4.h>
#include <net/netdev.h>
#include <net/packet.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sync/spin_lock.h>

void spin_lock(spinlock_t *lock)
{
    (void)lock;
}

void spin_unlock(spinlock_t *lock)
{
    (void)lock;
}

static int failures;

#define CHECK(condition, message)                                    \
    do {                                                             \
        if (!(condition)) {                                          \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, message); \
            failures++;                                              \
            return;                                                  \
        }                                                            \
    } while (0)

static void store_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void store_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

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

static uint16_t fixture_checksum(const void *data, size_t length)
{
    return checksum_finish(checksum_add(0, data, length));
}

static uint16_t fixture_ipv4_pseudo_checksum(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, size_t length)
{
    uint8_t pseudo[12];
    uint32_t sum;

    store_be32(pseudo, source);
    store_be32(pseudo + 4, destination);
    pseudo[8] = 0;
    pseudo[9] = protocol;
    store_be16(pseudo + 10, (uint16_t)length);
    sum = checksum_add(0, pseudo, sizeof(pseudo));
    return checksum_finish(checksum_add(sum, data, length));
}

static void finish_checksum(uint8_t *bytes, size_t length, size_t offset)
{
    bytes[offset] = 0;
    bytes[offset + 1] = 0;
    store_be16(bytes + offset, fixture_checksum(bytes, length));
}

static void make_ipv4(uint8_t *packet, size_t header_len, uint8_t protocol, size_t payload_len)
{
    memset(packet, 0, header_len + payload_len);
    packet[0] = (uint8_t)(0x40 | header_len / 4);
    store_be16(packet + 2, (uint16_t)(header_len + payload_len));
    store_be16(packet + 4, 0x1234);
    store_be16(packet + 6, 0x4000);
    packet[8] = 64;
    packet[9] = protocol;
    store_be32(packet + 12, 0xc0000201U);
    store_be32(packet + 16, 0xc6336402U);
    finish_checksum(packet, header_len, 10);
}

static void test_endian_helpers(void)
{
    CHECK(net_htons(0x1234) == 0x3412, "htons did not swap bytes on the host");
    CHECK(net_ntohs(net_htons(0xa1b2)) == 0xa1b2, "16-bit endian round trip");
    CHECK(net_htonl(0x01020304U) == 0x04030201U, "htonl did not swap bytes on the host");
    CHECK(net_ntohl(net_htonl(0x89abcdefU)) == 0x89abcdefU, "32-bit endian round trip");
}

static void test_checksum_vectors_and_odd_lengths(void)
{
    static const uint8_t rfc1071[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    static const uint8_t odd[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7, 0x01};
    static const uint8_t zeros[] = {0, 0, 0, 0};

    CHECK(net_checksum(NULL, 0) == 0xffff, "empty checksum is not one's complement zero");
    CHECK(net_checksum(zeros, sizeof(zeros)) == 0xffff, "all-zero checksum vector");
    CHECK(net_checksum(rfc1071, sizeof(rfc1071)) == 0x220d, "RFC 1071 checksum vector");
    CHECK(net_checksum(odd, sizeof(odd)) == 0x210d, "odd trailing byte was not high-order padded");
    CHECK(net_checksum_ipv4_pseudo(0xc0000201U, 0xc6336402U, 17, odd, sizeof(odd)) ==
              fixture_ipv4_pseudo_checksum(0xc0000201U, 0xc6336402U, 17, odd, sizeof(odd)),
          "IPv4 pseudo-header checksum vector");
}

static void test_ethernet_and_arp_validation(void)
{
    uint8_t ethernet[14 + 28] = {0};
    net_ethernet_frame_t frame;
    net_arp_packet_t arp;

    memset(ethernet, 0xff, 6);
    store_be16(ethernet + 12, 0x0806);
    store_be16(ethernet + 14, 1);
    store_be16(ethernet + 16, 0x0800);
    ethernet[18] = 6;
    ethernet[19] = 4;
    store_be16(ethernet + 20, 1);

    CHECK(net_ethernet_parse(ethernet, 13, &frame) < 0, "truncated Ethernet header accepted");
    CHECK(net_ethernet_parse(ethernet, sizeof(ethernet), &frame) == 0, "valid Ethernet frame rejected");
    CHECK(frame.ether_type == 0x0806 && frame.payload_len == 28 && frame.payload == ethernet + 14,
          "Ethernet view metadata is wrong");

    CHECK(net_arp_parse(frame.payload, 7, &arp) < 0, "truncated ARP fixed header accepted");
    CHECK(net_arp_parse(frame.payload, 27, &arp) < 0, "truncated ARP addresses accepted");
    CHECK(net_arp_parse(frame.payload, frame.payload_len, &arp) == 0, "valid Ethernet/IPv4 ARP rejected");
    CHECK(arp.hardware_type == 1 && arp.protocol_type == 0x0800 && arp.hardware_len == 6 && arp.protocol_len == 4 && arp.operation == 1,
          "ARP fields were not decoded in host order");
    ethernet[18] = 0;
    CHECK(net_arp_parse(frame.payload, frame.payload_len, &arp) < 0, "zero hardware-address length accepted");
    ethernet[18] = 255;
    CHECK(net_arp_parse(frame.payload, frame.payload_len, &arp) < 0, "overflowing ARP address geometry accepted");
}

static void test_ipv4_validation_options_and_checksum(void)
{
    uint8_t packet[28];
    net_ipv4_packet_t ip;

    make_ipv4(packet, 24, 17, 4);
    packet[20] = 1;
    packet[21] = 1;
    packet[22] = 0;
    packet[23] = 0;
    finish_checksum(packet, 24, 10);
    memcpy(packet + 24, "data", 4);

    CHECK(net_ipv4_parse(packet, 19, &ip) < 0, "truncated IPv4 fixed header accepted");
    CHECK(net_ipv4_parse(packet, sizeof(packet), &ip) == 0, "valid IPv4 options header rejected");
    CHECK(ip.header_len == 24 && ip.total_len == 28 && ip.protocol == 17 && ip.payload == packet + 24 && ip.payload_len == 4,
          "IPv4 options or payload boundary decoded incorrectly");
    CHECK(ip.source == 0xc0000201U && ip.destination == 0xc6336402U, "IPv4 addresses decoded incorrectly");

    packet[0] = 0x44;
    CHECK(net_ipv4_parse(packet, sizeof(packet), &ip) < 0, "IHL below five accepted");
    packet[0] = 0x47;
    CHECK(net_ipv4_parse(packet, sizeof(packet), &ip) < 0, "truncated IPv4 options accepted");
    packet[0] = 0x65;
    CHECK(net_ipv4_parse(packet, sizeof(packet), &ip) < 0, "non-IPv4 version accepted");
    packet[0] = 0x46;
    store_be16(packet + 2, 23);
    CHECK(net_ipv4_parse(packet, sizeof(packet), &ip) < 0, "total length below header length accepted");
    store_be16(packet + 2, 29);
    CHECK(net_ipv4_parse(packet, sizeof(packet), &ip) < 0, "total length beyond receive span accepted");
    store_be16(packet + 2, 28);
    packet[8] ^= 1;
    CHECK(net_ipv4_parse(packet, sizeof(packet), &ip) < 0, "bad IPv4 header checksum accepted");
}

static void make_udp(uint8_t *udp, const void *payload, size_t payload_len, uint16_t checksum)
{
    memset(udp, 0, 8 + payload_len);
    store_be16(udp, 12345);
    store_be16(udp + 2, 53);
    store_be16(udp + 4, (uint16_t)(8 + payload_len));
    memcpy(udp + 8, payload, payload_len);
    store_be16(udp + 6, checksum);
}

static void test_udp_lengths_and_zero_checksum_semantics(void)
{
    static const char payload[] = "abc";
    uint8_t udp[11];
    net_udp_datagram_t datagram;
    uint16_t checksum;

    make_udp(udp, payload, 3, 0);
    CHECK(net_udp_parse(udp, sizeof(udp), 0xc0000201U, 0xc6336402U, &datagram) == 0,
          "IPv4 UDP zero checksum was not accepted");
    CHECK(!datagram.checksum_present && datagram.source_port == 12345 && datagram.destination_port == 53 && datagram.payload_len == 3,
          "UDP zero-checksum view is wrong");
    CHECK(net_udp_parse(udp, 7, 0xc0000201U, 0xc6336402U, &datagram) < 0, "truncated UDP header accepted");
    store_be16(udp + 4, 7);
    CHECK(net_udp_parse(udp, sizeof(udp), 0xc0000201U, 0xc6336402U, &datagram) < 0, "UDP length below header accepted");
    store_be16(udp + 4, 12);
    CHECK(net_udp_parse(udp, sizeof(udp), 0xc0000201U, 0xc6336402U, &datagram) < 0, "UDP length beyond receive span accepted");

    make_udp(udp, payload, 3, 0);
    checksum = fixture_ipv4_pseudo_checksum(0xc0000201U, 0xc6336402U, 17, udp, sizeof(udp));
    store_be16(udp + 6, checksum ? checksum : 0xffff);
    CHECK(net_udp_parse(udp, sizeof(udp), 0xc0000201U, 0xc6336402U, &datagram) == 0 && datagram.checksum_present,
          "valid odd-length UDP checksum rejected");
    udp[10] ^= 1;
    CHECK(net_udp_parse(udp, sizeof(udp), 0xc0000201U, 0xc6336402U, &datagram) < 0, "bad UDP checksum accepted");
}

static void make_tcp(uint8_t *tcp, size_t length)
{
    memset(tcp, 0, length);
    store_be16(tcp, 443);
    store_be16(tcp + 2, 49152);
    store_be32(tcp + 4, 0xfffffff0U);
    store_be32(tcp + 8, 0x12345678U);
    tcp[12] = 5U << 4;
    tcp[13] = 0x12;
    store_be16(tcp + 14, 4096);
    store_be16(tcp + 16, fixture_ipv4_pseudo_checksum(0xc0000201U, 0xc6336402U, 6, tcp, length));
}

static void test_tcp_validation_state_and_sequence_wrap(void)
{
    uint8_t tcp[20];
    net_tcp_segment_t segment;

    make_tcp(tcp, sizeof(tcp));
    CHECK(net_tcp_parse(tcp, 19, 0xc0000201U, 0xc6336402U, &segment) < 0, "truncated TCP fixed header accepted");
    CHECK(net_tcp_parse(tcp, sizeof(tcp), 0xc0000201U, 0xc6336402U, &segment) == 0, "valid TCP segment rejected");
    CHECK(segment.source_port == 443 && segment.destination_port == 49152 && segment.sequence == 0xfffffff0U &&
              segment.acknowledgment == 0x12345678U && segment.header_len == 20 && segment.flags == 0x12,
          "TCP fields decoded incorrectly");
    tcp[12] = 4U << 4;
    CHECK(net_tcp_parse(tcp, sizeof(tcp), 0xc0000201U, 0xc6336402U, &segment) < 0, "TCP data offset below five accepted");
    tcp[12] = 6U << 4;
    CHECK(net_tcp_parse(tcp, sizeof(tcp), 0xc0000201U, 0xc6336402U, &segment) < 0, "truncated TCP options accepted");
    make_tcp(tcp, sizeof(tcp));
    tcp[4] ^= 1;
    CHECK(net_tcp_parse(tcp, sizeof(tcp), 0xc0000201U, 0xc6336402U, &segment) < 0, "bad TCP checksum accepted");

    CHECK(net_tcp_seq_before(0xfffffff0U, 0x00000010U), "sequence wrap was not ordered before");
    CHECK(net_tcp_seq_after(0x00000010U, 0xfffffff0U), "sequence wrap was not ordered after");
    CHECK(!net_tcp_seq_before(7, 7) && !net_tcp_seq_after(7, 7), "equal TCP sequence numbers were ordered");
    CHECK(net_tcp_state_next(TCP_STATE_CLOSED, TCP_EVENT_ACTIVE_OPEN) == TCP_STATE_SYN_SENT, "active open transition");
    CHECK(net_tcp_state_next(TCP_STATE_LISTEN, TCP_EVENT_RX_SYN) == TCP_STATE_SYN_RECEIVED, "passive SYN transition");
    CHECK(net_tcp_state_next(TCP_STATE_SYN_SENT, TCP_EVENT_RX_SYN_ACK) == TCP_STATE_ESTABLISHED, "SYN-ACK transition");
    CHECK(net_tcp_state_next(TCP_STATE_ESTABLISHED, TCP_EVENT_CLOSE) == TCP_STATE_FIN_WAIT_1, "active close transition");
    CHECK(net_tcp_state_next(TCP_STATE_ESTABLISHED, TCP_EVENT_RX_FIN) == TCP_STATE_CLOSE_WAIT, "passive close transition");
    CHECK(net_tcp_state_next(TCP_STATE_LAST_ACK, TCP_EVENT_RX_ACK) == TCP_STATE_CLOSED, "last ACK transition");
}

typedef struct mock_driver {
    int calls;
    int result;
    net_packet_t *seen;
} mock_driver_t;

typedef struct release_tracker {
    int calls;
    void *last_data;
} release_tracker_t;

static void packet_released(void *context, void *data)
{
    release_tracker_t *tracker = context;
    tracker->calls++;
    tracker->last_data = data;
}

static int mock_transmit(netdev_t *device, net_packet_t *packet)
{
    mock_driver_t *mock = netdev_private(device);
    mock->calls++;
    mock->seen = packet;
    return mock->result;
}

static const netdev_ops_t mock_ops = {.transmit = mock_transmit};

static void test_packet_reference_ownership(void)
{
    uint8_t storage[32];
    release_tracker_t tracker = {0};
    net_packet_t packet;

    CHECK(net_packet_init_external(&packet, storage, sizeof(storage), packet_released, &tracker) == 0, "external packet init");
    CHECK(net_packet_data(&packet) == storage && net_packet_length(&packet) == sizeof(storage), "external packet span");
    net_packet_get(&packet);
    net_packet_put(&packet);
    CHECK(tracker.calls == 0, "packet released while a reference remained");
    net_packet_put(&packet);
    CHECK(tracker.calls == 1 && tracker.last_data == storage, "packet was not released exactly at last put");
}

static void test_netdev_registry_and_transmit_cleanup(void)
{
    uint8_t success_data[8], error_data[8], rejected_data[8];
    release_tracker_t success = {0}, error = {0}, rejected = {0};
    mock_driver_t mock = {0};
    net_packet_t success_packet, error_packet, rejected_packet;
    netdev_t device, duplicate;

    CHECK(netdev_init(&device, "eth-test0", &mock_ops, &mock) == 0, "netdev init");
    CHECK(netdev_init(&duplicate, "eth-test0", &mock_ops, &mock) == 0, "duplicate netdev init");
    CHECK(netdev_register(&device) == 0, "netdev registration");
    CHECK(netdev_register(&device) < 0, "double registration accepted");
    CHECK(netdev_register(&duplicate) < 0, "duplicate netdev name accepted");
    netdev_t *found = netdev_find("eth-test0");
    CHECK(found == &device, "registered netdev lookup failed");
    netdev_put(found);
    CHECK(netdev_find("missing0") == NULL, "missing netdev lookup succeeded");

    net_packet_init_external(&success_packet, success_data, sizeof(success_data), packet_released, &success);
    mock.result = 0;
    CHECK(netdev_transmit(&device, &success_packet) == 0, "successful transmit returned an error");
    CHECK(mock.calls == 1 && mock.seen == &success_packet, "driver did not receive packet");
    CHECK(success.calls == 1, "successful transmit leaked its consumed packet");

    net_packet_init_external(&error_packet, error_data, sizeof(error_data), packet_released, &error);
    mock.result = -EIO;
    CHECK(netdev_transmit(&device, &error_packet) == -EIO, "driver transmit error was lost");
    CHECK(mock.calls == 2 && error.calls == 1, "driver error path leaked its consumed packet");

    netdev_get(&device);
    CHECK(netdev_unregister(&device) == 0, "netdev unregister");
    CHECK(netdev_find("eth-test0") == NULL, "unregistered netdev remained discoverable");
    CHECK(netdev_unregister(&device) < 0, "double unregister accepted");
    net_packet_init_external(&rejected_packet, rejected_data, sizeof(rejected_data), packet_released, &rejected);
    CHECK(netdev_transmit(&device, &rejected_packet) < 0, "transmit on unregistered netdev accepted");
    CHECK(mock.calls == 2 && rejected.calls == 1, "core rejection called driver or leaked packet");
    netdev_put(&device);
}

int main(void)
{
    test_endian_helpers();
    test_checksum_vectors_and_odd_lengths();
    test_ethernet_and_arp_validation();
    test_ipv4_validation_options_and_checksum();
    test_udp_lengths_and_zero_checksum_semantics();
    test_tcp_validation_state_and_sequence_wrap();
    test_packet_reference_ownership();
    test_netdev_registry_and_transmit_cleanup();

    if (failures) {
        printf("%d net core test(s) failed\n", failures);
        return 1;
    }
    printf("PASS net wire validation, checksums, TCP state, and ownership semantics\n");
    return 0;
}

#endif
