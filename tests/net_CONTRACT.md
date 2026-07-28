# Net core host-test contract

`make -C tests net-test` is intentionally compile-gated until the net core is
available. The tests expect these public headers:

- `net/byteorder.h`
- `net/checksum.h`
- `net/ethernet.h`
- `net/arp.h`
- `net/ipv4.h`
- `net/udp.h`
- `net/tcp.h`
- `net/packet.h`
- `net/netdev.h`

The test names and calls in `net_core_test.c` are the API contract. Parsers
return zero for a valid packet and a negative errno for malformed or truncated
input. They must not read beyond the supplied span. Multi-byte fields exposed
by parsed views, including IPv4 addresses, are in host byte order. The
pseudo-header checksum helper accepts addresses in that same representation.

Required pure helpers are:

```c
uint16_t net_htons(uint16_t value);
uint16_t net_ntohs(uint16_t value);
uint32_t net_htonl(uint32_t value);
uint32_t net_ntohl(uint32_t value);
uint16_t net_checksum(const void *data, size_t length);
uint16_t net_checksum_ipv4_pseudo(uint32_t source, uint32_t destination,
                                  uint8_t protocol, const void *data,
                                  size_t length);
bool net_tcp_seq_before(uint32_t a, uint32_t b);
bool net_tcp_seq_after(uint32_t a, uint32_t b);
tcp_state_t net_tcp_state_next(tcp_state_t state, tcp_event_t event);
```

The parser view types and fields used by the tests are:

```c
net_ethernet_frame_t: ether_type, payload, payload_len
net_arp_packet_t: hardware_type, protocol_type, hardware_len, protocol_len,
                  operation
net_ipv4_packet_t: header_len, total_len, protocol, source, destination,
                   payload, payload_len
net_udp_datagram_t: source_port, destination_port, payload, payload_len,
                    checksum_present
net_tcp_segment_t: source_port, destination_port, sequence, acknowledgment,
                   header_len, flags, payload, payload_len
```

Parser entry points are `net_ethernet_parse`, `net_arp_parse`,
`net_ipv4_parse`, `net_udp_parse`, and `net_tcp_parse`. UDP and TCP parsers take
source and destination IPv4 addresses after the byte span so they can validate
their pseudo-header checksums.

Packet/netdev lifecycle uses `net_packet_init_external`, `net_packet_get`,
`net_packet_put`, `netdev_init`, `netdev_register`, `netdev_unregister`,
`netdev_find`, `netdev_get`, `netdev_put`, and `netdev_transmit`. An external
packet's release callback runs exactly once at the last reference.
`netdev_find` returns a reference that must be put. Registration holds one
reference until unregister. Device names are unique. `netdev_transmit` consumes
the caller's packet reference on success, driver error, or core rejection; the
driver callback borrows that reference and must not put it.

TCP state tests cover only deterministic transitions and serial-number
arithmetic. Timers, retransmission, and socket integration belong in a later
clock-driven suite.
