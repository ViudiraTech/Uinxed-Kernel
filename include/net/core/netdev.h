/*
 *
 *      netdev.h
 *      Network device API
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_NETDEV_H_
#define INCLUDE_NETDEV_H_

#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <net/core/pbuf.h>

#define ARPHRD_ETHER    1U
#define ARPHRD_LOOPBACK 772U

#define NETDEV_MTU_MIN     576U
#define NETDEV_F_UP        0x0001U
#define NETDEV_F_RUNNING   0x0002U
#define NETDEV_F_BROADCAST 0x0004U
#define NETDEV_F_PROMISC   0x0008U
#define NETDEV_F_LOOPBACK  0x0010U

typedef struct net_device net_device_t;
typedef void (*netdev_iter_fn)(net_device_t *device, void *context);

typedef enum netdev_lifecycle_event {
    NETDEV_REGISTERED,
    NETDEV_UNREGISTERED,
} netdev_lifecycle_event_t;

typedef void (*netdev_lifecycle_fn)(net_device_t *device, netdev_lifecycle_event_t event, void *context);

typedef struct netdev_stats {
        uint64_t rx_packets;
        uint64_t rx_bytes;
        uint64_t rx_dropped;
        uint64_t rx_errors;
        uint64_t tx_packets;
        uint64_t tx_bytes;
        uint64_t tx_dropped;
        uint64_t tx_errors;
} netdev_stats_t;

typedef struct netdev_ops {
        int (*open)(net_device_t *device);
        void (*stop)(net_device_t *device);
        int (*xmit)(net_device_t *device, net_pbuf_t *packet);
        int (*set_mtu)(net_device_t *device, uint32_t mtu);
} netdev_ops_t;

struct net_device {
        char                name[CONFIG_NETDEV_NAME_MAX];
        uint8_t             address[6];
        uint32_t            mtu;
        uint32_t            flags;
        uint32_t            ipv4_address;
        uint32_t            ipv4_netmask;
        uint32_t            ipv4_gateway;
        uint32_t            ipv4_dns[CONFIG_NETDEV_DNS_MAX];
        uint8_t             ipv6_link_local[16];
        uint8_t             ipv6_address[16];
        uint8_t             ipv6_prefix_length;
        uint8_t             ipv6_default_router[16];
        uint32_t            ipv6_mtu;
        uint64_t            ipv6_valid_until;
        uint64_t            ipv6_preferred_until;
        uint64_t            ipv6_router_until;
        const netdev_ops_t *ops;
        void               *driver_data;
        netdev_stats_t      stats;
        uint32_t            ifindex;
        uint32_t            refs;
        uint8_t             registered;
        spinlock_t          lock;
};

/* Network stack periodic timer. */
#if CONFIG_NET
void net_timer(uint64_t now_ticks);
#else
static inline void net_timer(uint64_t) {}
#endif

/* Device registry and lookup. */
int           netdev_register(net_device_t *device);
int           netdev_unregister(net_device_t *device);
net_device_t *netdev_get_by_name(const char *name);
net_device_t *netdev_get_default(void);

#if CONFIG_NET
void netdev_iterate(netdev_iter_fn callback, void *context);
int  netdev_set_lifecycle_notifier(netdev_lifecycle_fn callback, void *context);
#else
static inline void netdev_iterate(netdev_iter_fn, void *) {}
static inline int  netdev_set_lifecycle_notifier(netdev_lifecycle_fn, void *)
{
    return -ENOSYS;
}
#endif

void netdev_put(net_device_t *device);

/* Device configuration. */
int    netdev_set_up(net_device_t *device, int up);
int    netdev_set_mtu(net_device_t *device, uint32_t mtu);
int    netdev_configure_ipv4(net_device_t *device, uint32_t address, uint32_t netmask, uint32_t gateway);
int    netdev_configure_dns(net_device_t *device, const uint32_t *servers, size_t count);
size_t netdev_get_dns_servers(net_device_t *device, uint32_t *servers, size_t capacity);
int    netdev_udp_broadcast(net_device_t *device, uint32_t source, uint16_t source_port, uint16_t destination_port, const void *data, size_t length);

#if CONFIG_NET
void netdev_get_stats(net_device_t *device, netdev_stats_t *stats);
#else
static inline void netdev_get_stats(net_device_t *, netdev_stats_t *stats)
{
    if (stats) *stats = (netdev_stats_t) {0};
}
#endif

/* RX consumes packet on every return path. TX does not consume packet. */
int netdev_rx(net_device_t *device, net_pbuf_t *packet);
int netdev_tx(net_device_t *device, net_pbuf_t *packet);

/* Driver-facing init and accessors. */
int           netdev_init(net_device_t *device, const char *name, const netdev_ops_t *ops, void *private_data);
net_device_t *netdev_find(const char *name);
void          netdev_get(net_device_t *device);
void         *netdev_private(net_device_t *device);

#endif // INCLUDE_NETDEV_H_
