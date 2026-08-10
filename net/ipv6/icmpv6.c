/*
 *
 *      icmpv6.c
 *      ICMPv6 protocol implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <net/core/endian.h>
#include <net/ipv6/icmpv6.h>
#include <net/ipv6/ndp.h>
#include <process/sched.h>

#define ICMPV6_HEADER_LEN 8U

static int icmpv6_is_error(uint8_t type)
{
    return type < 128U;
}

int icmpv6_input(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || packet->length < ICMPV6_HEADER_LEN
        || net_checksum_ipv6_pseudo(&ip->source, &ip->destination, IPV6_NEXT_ICMP, packet->data, packet->length) != 0)
        goto bad;
    uint8_t type = packet->data[0];
    uint8_t code = packet->data[1];
    if (type == ICMPV6_ECHO_REQUEST) {
        if (code || ipv6_address_is_unspecified(&ip->source)) goto bad;
        packet->data[0] = ICMPV6_ECHO_REPLY;
        packet->data[2] = packet->data[3] = 0;
        ipv6_address_t source             = ip->destination;
        if (ipv6_address_is_multicast(&source)) memcpy(source.bytes, device->ipv6_link_local, IPV6_ADDRESS_LEN);
        uint16_t checksum = net_checksum_ipv6_pseudo(&source, &ip->source, IPV6_NEXT_ICMP, packet->data, packet->length);
        net_write_be16(packet->data + 2, checksum ? checksum : UINT16_MAX);
        int status = ipv6_output(device, &source, &ip->source, IPV6_NEXT_ICMP, 64, packet);
        net_pbuf_free(packet);
        return status;
    }
    if (type >= ICMPV6_ROUTER_SOLICIT && type <= ICMPV6_NEIGHBOR_ADVERT) return ndp_input(device, ip, packet);
    if (icmpv6_is_error(type) && packet->length >= ICMPV6_HEADER_LEN + IPV6_HEADER_LEN) {
        uint32_t value = net_read_be32(packet->data + 4);
        ipv6_control_error(type, code, value, packet->data + ICMPV6_HEADER_LEN, packet->length - ICMPV6_HEADER_LEN);
    }
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

int icmpv6_error(net_device_t *device, const ipv6_address_t *destination, uint8_t type, uint8_t code, uint32_t value, const void *original,
                 size_t original_length)
{
    if (!device || !destination || !ipv6_address_is_unicast(destination) || !original || original_length < IPV6_HEADER_LEN
        || (type != ICMPV6_DEST_UNREACHABLE && type != ICMPV6_PACKET_TOO_BIG && type != ICMPV6_TIME_EXCEEDED
            && type != ICMPV6_PARAMETER_PROBLEM))
        return -EINVAL;
    const uint8_t *ip = original;
    ipv6_address_t original_destination;
    memcpy(original_destination.bytes, ip + 24, IPV6_ADDRESS_LEN);
    if ((ip[0] >> 4) != 6 || ipv6_address_is_multicast(&original_destination)) return -EINVAL;
    net_ipv6_packet_t parsed;
    if (!net_ipv6_parse(original, original_length, &parsed) && parsed.protocol == IPV6_NEXT_ICMP && parsed.payload_len
        && icmpv6_is_error(parsed.payload[0]))
        return -EINVAL;
    size_t quote_length = original_length;
    if (quote_length > IPV6_MIN_MTU - IPV6_HEADER_LEN - ICMPV6_HEADER_LEN) quote_length = IPV6_MIN_MTU - IPV6_HEADER_LEN - ICMPV6_HEADER_LEN;
    net_pbuf_t *packet = net_pbuf_alloc(ICMPV6_HEADER_LEN + quote_length, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("icmpv6: Error message alloc failed (type=%u code=%u)\n", (unsigned)type, (unsigned)code);
        return -ENOMEM;
    }
    memset(packet->data, 0, ICMPV6_HEADER_LEN);
    packet->data[0] = type;
    packet->data[1] = code;
    net_write_be32(packet->data + 4, value);
    memcpy(packet->data + ICMPV6_HEADER_LEN, original, quote_length);
    ipv6_address_t source;
    memcpy(source.bytes, device->ipv6_link_local, IPV6_ADDRESS_LEN);
    uint16_t checksum = net_checksum_ipv6_pseudo(&source, destination, IPV6_NEXT_ICMP, packet->data, packet->length);
    net_write_be16(packet->data + 2, checksum ? checksum : UINT16_MAX);
    int status = ipv6_output(device, &source, destination, IPV6_NEXT_ICMP, 64, packet);
    net_pbuf_free(packet);
    return status;
}
