/*
 *
 *      ethernet.c
 *      Ethernet driver
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <net/core/endian.h>
#include <net/core/ethernet.h>
#include <net/ipv4/arp.h>
#include <net/ipv4/ipv4.h>
#include <net/ipv6/ipv6.h>

const uint8_t ethernet_broadcast_address[ETH_ADDRESS_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static int ethernet_address_valid(const uint8_t address[ETH_ADDRESS_LEN])
{
    static const uint8_t zero[ETH_ADDRESS_LEN];
    return memcmp(address, zero, ETH_ADDRESS_LEN) != 0;
}

int net_ethernet_parse(const void *data, size_t length, net_ethernet_frame_t *frame)
{
    if (!data || !frame || length < ETH_HEADER_LEN) return -EBADMSG;
    const uint8_t *bytes = data;
    frame->ether_type    = net_read_be16(bytes + 12);
    frame->payload       = bytes + ETH_HEADER_LEN;
    frame->payload_len   = length - ETH_HEADER_LEN;
    return 0;
}

int ethernet_input(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet || packet->length <= ETH_HEADER_LEN) {
        net_pbuf_free(packet);
        return -EBADMSG;
    }
    const uint8_t *header = packet->data;
    uint16_t       type   = net_read_be16(header + 12);
    if (!ethernet_address_valid(header + ETH_ADDRESS_LEN) || (header[ETH_ADDRESS_LEN] & 1U)
        || (!(device->flags & NETDEV_F_PROMISC) && memcmp(header, device->address, ETH_ADDRESS_LEN) != 0
            && memcmp(header, ethernet_broadcast_address, ETH_ADDRESS_LEN) != 0 && !(header[0] == 0x33 && header[1] == 0x33))) {
        net_pbuf_free(packet);
        return -EHOSTUNREACH;
    }
    net_pbuf_pull(packet, ETH_HEADER_LEN);
    if (type == ETH_TYPE_ARP) return arp_input(device, packet);
    if (type == ETH_TYPE_IPV4) return ipv4_input(device, packet);
    if (type == ETH_TYPE_IPV6) return ipv6_input(device, packet);
    net_pbuf_free(packet);
    return -EPROTONOSUPPORT;
}

int ethernet_output(net_device_t *device, net_pbuf_t *packet, const uint8_t destination[6], uint16_t type)
{
    if (!device || !packet || !destination || !ethernet_address_valid(device->address) || !ethernet_address_valid(destination)) return -EINVAL;
    uint8_t *header = net_pbuf_push(packet, ETH_HEADER_LEN);
    if (!header) {
        plogk("ethernet: %s: Output header push failed (%d bytes)\n", device->name, ETH_HEADER_LEN);
        return -ENOBUFS;
    }
    memcpy(header, destination, ETH_ADDRESS_LEN);
    memcpy(header + 6, device->address, ETH_ADDRESS_LEN);
    net_write_be16(header + 12, type);
    return netdev_tx(device, packet);
}
