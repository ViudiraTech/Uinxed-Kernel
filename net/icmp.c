#include <kernel/errno.h>
#include <libs/std/string.h>
#include <net/endian.h>
#include <net/icmp.h>

#define ICMP_HEADER_LEN 8U
#define ICMP_QUOTE_LEN  8U

static int icmp_is_error(uint8_t type)
{
    return type == ICMP_DEST_UNREACHABLE || type == 4U || type == 5U || type == ICMP_TIME_EXCEEDED || type == 12U;
}

int icmp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || packet->length < ICMP_HEADER_LEN || net_checksum(packet->data, packet->length) != 0) goto bad;
    uint8_t type = packet->data[0];
    uint8_t code = packet->data[1];
    if (type == ICMP_ECHO_REQUEST) {
        if (code || ip->destination == UINT32_MAX || (device->ipv4_netmask && ip->destination == (device->ipv4_address | ~device->ipv4_netmask)))
            goto ignored;
        packet->data[0] = ICMP_ECHO_REPLY;
        packet->data[2] = packet->data[3] = 0;
        net_write_be16(packet->data + 2, net_checksum(packet->data, packet->length));
        int status = ipv4_output(device, device->ipv4_address, ip->source, IPV4_PROTO_ICMP, 64, packet);
        net_pbuf_free(packet);
        return status;
    }
    if ((type == ICMP_DEST_UNREACHABLE || type == ICMP_TIME_EXCEEDED) && packet->length >= ICMP_HEADER_LEN + IPV4_HEADER_MIN) {
        uint32_t mtu = type == ICMP_DEST_UNREACHABLE && code == ICMP_FRAGMENTATION_NEEDED ? net_read_be16(packet->data + 6) : 0;
        ipv4_control_error(type, code, mtu, packet->data + ICMP_HEADER_LEN, packet->length - ICMP_HEADER_LEN);
    }
ignored:
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

int icmp_error_mtu(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, uint16_t mtu, const void *original,
                   size_t original_length)
{
    if (!device || !destination || !original || original_length < IPV4_HEADER_MIN) return -EINVAL;
    const uint8_t *ip = original;
    size_t header_length = (size_t)(ip[0] & 0x0fU) * 4U;
    if ((ip[0] >> 4) != 4 || header_length < IPV4_HEADER_MIN || header_length > original_length || (net_read_be16(ip + 6) & IPV4_FRAGMENT_MASK)
        || destination == UINT32_MAX || (device->ipv4_netmask && destination == (device->ipv4_address | ~device->ipv4_netmask)))
        return -EINVAL;
    if (ip[9] == IPV4_PROTO_ICMP && original_length > header_length && icmp_is_error(ip[header_length])) return -EINVAL;
    size_t quote_length = header_length + ICMP_QUOTE_LEN;
    if (quote_length > original_length) quote_length = original_length;
    net_pbuf_t *packet = net_pbuf_alloc(ICMP_HEADER_LEN + quote_length, NET_PBUF_HEADROOM);
    if (!packet) return -ENOMEM;
    memset(packet->data, 0, ICMP_HEADER_LEN);
    packet->data[0] = type;
    packet->data[1] = code;
    if (type == ICMP_DEST_UNREACHABLE && code == ICMP_FRAGMENTATION_NEEDED) net_write_be16(packet->data + 6, mtu);
    memcpy(packet->data + ICMP_HEADER_LEN, original, quote_length);
    net_write_be16(packet->data + 2, net_checksum(packet->data, packet->length));
    int status = ipv4_output(device, device->ipv4_address, destination, IPV4_PROTO_ICMP, 64, packet);
    net_pbuf_free(packet);
    return status;
}

int icmp_error(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, const void *original, size_t original_length)
{
    return icmp_error_mtu(device, destination, type, code, 0, original, original_length);
}
