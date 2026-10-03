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

#define DHCP_SERVER_PORT        67U
#define DHCP_CLIENT_PORT        68U
#define DHCP_FIXED_LENGTH       240U
#define DHCP_MAGIC_COOKIE       0x63825363U
#define DHCP_OPT_PAD            0U
#define DHCP_OPT_NETMASK        1U
#define DHCP_OPT_ROUTER         3U
#define DHCP_OPT_DNS            6U
#define DHCP_OPT_REQUESTED_IP   50U
#define DHCP_OPT_LEASE          51U
#define DHCP_OPT_MESSAGE_TYPE   53U
#define DHCP_OPT_SERVER_ID      54U
#define DHCP_OPT_PARAMETER_LIST 55U
#define DHCP_OPT_MAX_MESSAGE    57U
#define DHCP_OPT_RENEWAL        58U
#define DHCP_OPT_REBINDING      59U
#define DHCP_OPT_CLIENT_ID      61U
#define DHCP_OPT_END            255U
#define DHCP_DISCOVER           1U
#define DHCP_OFFER              2U
#define DHCP_REQUEST            3U
#define DHCP_ACK                5U
#define DHCP_NAK                6U

typedef struct dhcp_reply {
        uint32_t offered_address;
        uint32_t server_identifier;
        uint32_t netmask;
        uint32_t gateway;
        uint32_t dns[CONFIG_NETDEV_DNS_MAX];
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
#if CONFIG_INET && CONFIG_NET
void dhcp_init(void);
void dhcp_timer(uint64_t now_ticks);
void dhcp_device_removed(net_device_t *device);
#else
static inline void dhcp_init(void) {}
static inline void dhcp_timer(uint64_t) {}
static inline void dhcp_device_removed(net_device_t *) {}
#endif

/* Parses and validates a BOOTP/DHCP reply for one transaction and client. */
int dhcp_parse_reply(const void *data, size_t length, uint32_t expected_xid, const uint8_t hardware_address[6], dhcp_reply_t *reply);

#endif // INCLUDE_DHCP_H_
