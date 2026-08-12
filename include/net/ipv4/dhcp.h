/*
 *
 *      dhcp.h
 *      DHCP protocol definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DHCP_H_
#define INCLUDE_DHCP_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <net/core/netdev.h>

typedef struct dhcp_reply {
        uint32_t offered_address;
        uint32_t server_identifier;
        uint32_t netmask;
        uint32_t gateway;
        uint32_t dns[NETDEV_DNS_MAX];
        uint32_t lease_seconds;
        uint32_t renewal_seconds;
        uint32_t rebinding_seconds;
        uint8_t  message_type;
        uint8_t  dns_count;
        uint8_t  has_netmask;
        uint8_t  has_gateway;
        uint8_t  has_dns;
        uint8_t  has_lease;
        uint8_t  has_renewal;
        uint8_t  has_rebinding;
} dhcp_reply_t;

/* DHCP client lifecycle and periodic state machine. */
void dhcp_init(void);
void dhcp_timer(uint64_t now_ticks);
void dhcp_device_removed(net_device_t *device);

/* Parses and validates a BOOTP/DHCP reply for one transaction and client. */
int dhcp_parse_reply(const void *data, size_t length, uint32_t expected_xid, const uint8_t hardware_address[6], dhcp_reply_t *reply);

#endif // INCLUDE_DHCP_H_
