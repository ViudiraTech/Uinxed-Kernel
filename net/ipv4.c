#include <kernel/errno.h>
#include <libs/std/string.h>
#include <net/arp.h>
#include <net/endian.h>
#include <net/icmp.h>
#include <net/ipv4.h>
#include <net/tcp.h>
#include <net/udp.h>

static uint16_t ipv4_id;
static spinlock_t ipv4_id_lock;

int net_ipv4_parse(const void *data, size_t length, net_ipv4_packet_t *packet)
{
    if (!data || !packet || length < IPV4_HEADER_MIN) return -EBADMSG;
    const uint8_t *bytes = data;
    size_t header_length = (size_t)(bytes[0] & 0x0fU) * 4U;
    if ((bytes[0] >> 4) != 4 || header_length < IPV4_HEADER_MIN || header_length > length) return -EBADMSG;
    uint16_t total = net_read_be16(bytes + 2);
    if (total < header_length || total > length || net_checksum(bytes, header_length) != 0) return -EBADMSG;
    packet->header_len = (uint8_t)header_length;
    packet->total_len = total;
    packet->protocol = bytes[9];
    packet->source = net_read_be32(bytes + 12);
    packet->destination = net_read_be32(bytes + 16);
    packet->payload = bytes + header_length;
    packet->payload_len = total - header_length;
    return 0;
}

int ipv4_route(uint32_t destination, net_device_t **device, uint32_t *next_hop)
{
    if (!device || !next_hop) return -EINVAL;
    net_device_t *candidate = netdev_get_default();
    if (!candidate) return -ENETUNREACH;
    if ((destination & candidate->ipv4_netmask) == (candidate->ipv4_address & candidate->ipv4_netmask)) *next_hop = destination;
    else if (candidate->ipv4_gateway) *next_hop = candidate->ipv4_gateway;
    else {
        netdev_put(candidate);
        return -ENETUNREACH;
    }
    *device = candidate;
    return 0;
}

int ipv4_output(net_device_t *device, uint32_t source, uint32_t destination, uint8_t protocol, uint8_t ttl, net_pbuf_t *packet)
{
    if (!packet || !destination || packet->length > UINT16_MAX - IPV4_HEADER_MIN) return -EMSGSIZE;
    uint32_t next_hop;
    int release = 0;
    if (!device) {
        int status = ipv4_route(destination, &device, &next_hop);
        if (status) return status;
        release = 1;
    } else next_hop = ((destination & device->ipv4_netmask) == (device->ipv4_address & device->ipv4_netmask)) ? destination : device->ipv4_gateway;
    if (!source) source = device->ipv4_address;
    if (!source || !next_hop || packet->length + IPV4_HEADER_MIN > device->mtu) {
        if (release) netdev_put(device);
        return packet->length + IPV4_HEADER_MIN > device->mtu ? -EMSGSIZE : -ENETUNREACH;
    }
    uint8_t *header = net_pbuf_push(packet, IPV4_HEADER_MIN);
    if (!header) {
        if (release) netdev_put(device);
        return -ENOBUFS;
    }
    memset(header, 0, IPV4_HEADER_MIN);
    header[0] = 0x45;
    net_write_be16(header + 2, (uint16_t)packet->length);
    spin_lock(&ipv4_id_lock);
    net_write_be16(header + 4, ++ipv4_id);
    spin_unlock(&ipv4_id_lock);
    net_write_be16(header + 6, 0x4000);
    header[8] = ttl ? ttl : 64;
    header[9] = protocol;
    net_write_be32(header + 12, source);
    net_write_be32(header + 16, destination);
    net_write_be16(header + 10, net_checksum(header, IPV4_HEADER_MIN));
    int status = arp_resolve(device, next_hop, packet);
    net_pbuf_pull(packet, IPV4_HEADER_MIN);
    if (release) netdev_put(device);
    return status;
}

int ipv4_input(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet || packet->length < IPV4_HEADER_MIN) goto bad;
    uint8_t *header = packet->data;
    size_t header_length = (size_t)(header[0] & 0x0fU) * 4U;
    if ((header[0] >> 4) != 4 || header_length < IPV4_HEADER_MIN || header_length > packet->length) goto bad;
    uint16_t total = net_read_be16(header + 2);
    if (total < header_length || total > packet->length || net_checksum(header, header_length) != 0) goto bad;
    uint16_t fragment = net_read_be16(header + 6);
    if (fragment & 0x3fffU) {
        net_pbuf_free(packet);
        return -EOPNOTSUPP;
    }
    ipv4_info_t info = {
        .source = net_read_be32(header + 12),
        .destination = net_read_be32(header + 16),
        .payload_length = (uint16_t)(total - header_length),
        .protocol = header[9],
        .ttl = header[8],
    };
    uint32_t broadcast = device->ipv4_address | ~device->ipv4_netmask;
    if (info.destination != device->ipv4_address && info.destination != broadcast && info.destination != UINT32_MAX) {
        net_pbuf_free(packet);
        return -EHOSTUNREACH;
    }
    net_pbuf_trim(packet, total);
    net_pbuf_pull(packet, header_length);
    if (info.protocol == IPV4_PROTO_ICMP) return icmp_input(device, &info, packet);
    if (info.protocol == IPV4_PROTO_UDP) return udp_input(device, &info, packet);
    if (info.protocol == IPV4_PROTO_TCP) return tcp_input(device, &info, packet);
    net_pbuf_free(packet);
    return -EPROTONOSUPPORT;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}
