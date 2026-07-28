#include <kernel/errno.h>
#include <libs/std/string.h>
#include <net/endian.h>
#include <net/icmp.h>

#define ICMP_HEADER_LEN 8U

int icmp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || packet->length < ICMP_HEADER_LEN || net_checksum(packet->data, packet->length) != 0) {
        net_pbuf_free(packet);
        return -EBADMSG;
    }
    if (packet->data[0] != ICMP_ECHO_REQUEST || packet->data[1] != 0) {
        net_pbuf_free(packet);
        return 0;
    }
    packet->data[0] = ICMP_ECHO_REPLY;
    packet->data[2] = packet->data[3] = 0;
    net_write_be16(packet->data + 2, net_checksum(packet->data, packet->length));
    int status = ipv4_output(device, device->ipv4_address, ip->source, IPV4_PROTO_ICMP, 64, packet);
    net_pbuf_free(packet);
    return status;
}

int icmp_error(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, const void *original, size_t original_length)
{
    if (!device || !destination || (!original && original_length)) return -EINVAL;
    if (original_length > 28) original_length = 28;
    net_pbuf_t *packet = net_pbuf_alloc(ICMP_HEADER_LEN + original_length, NET_PBUF_HEADROOM);
    if (!packet) return -ENOMEM;
    memset(packet->data, 0, ICMP_HEADER_LEN);
    packet->data[0] = type;
    packet->data[1] = code;
    if (original_length) memcpy(packet->data + ICMP_HEADER_LEN, original, original_length);
    net_write_be16(packet->data + 2, net_checksum(packet->data, packet->length));
    int status = ipv4_output(device, device->ipv4_address, destination, IPV4_PROTO_ICMP, 64, packet);
    net_pbuf_free(packet);
    return status;
}
