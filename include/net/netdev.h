#ifndef INCLUDE_NET_NETDEV_H_
#define INCLUDE_NET_NETDEV_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <net/packet.h>
#include <net/pbuf.h>
#include <sync/spin_lock.h>

#define NETDEV_NAME_MAX 16U
#define NETDEV_MAX      16U
#define NETDEV_MTU_MIN  576U
#define NETDEV_MTU_MAX  9000U
#define NETDEV_DNS_MAX  2U

#define NETDEV_F_UP        0x0001U
#define NETDEV_F_RUNNING   0x0002U
#define NETDEV_F_BROADCAST 0x0004U

typedef struct net_device net_device_t;
typedef net_device_t      netdev_t;
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
        union {
                int (*xmit)(net_device_t *device, net_pbuf_t *packet);
                int (*transmit)(netdev_t *device, net_packet_t *packet);
        };
        int (*set_mtu)(net_device_t *device, uint32_t mtu);
} netdev_ops_t;

struct net_device {
        char                name[NETDEV_NAME_MAX];
        uint8_t             address[6];
        uint32_t            mtu;
        uint32_t            flags;
        uint32_t            ipv4_address;
        uint32_t            ipv4_netmask;
        uint32_t            ipv4_gateway;
    uint32_t            ipv4_dns[NETDEV_DNS_MAX];
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

void          net_init(void);
void          net_timer(uint64_t now_ticks);
int           netdev_register(net_device_t *device);
int           netdev_unregister(net_device_t *device);
net_device_t *netdev_get_by_name(const char *name);
net_device_t *netdev_get_default(void);
void          netdev_iterate(netdev_iter_fn callback, void *context);
int           netdev_set_lifecycle_notifier(netdev_lifecycle_fn callback, void *context);
void          netdev_put(net_device_t *device);
int           netdev_set_up(net_device_t *device, int up);
int           netdev_set_mtu(net_device_t *device, uint32_t mtu);
int           netdev_configure_ipv4(net_device_t *device, uint32_t address, uint32_t netmask, uint32_t gateway);
int           netdev_configure_dns(net_device_t *device, const uint32_t *servers, size_t count);
size_t        netdev_get_dns_servers(net_device_t *device, uint32_t *servers, size_t capacity);
int           netdev_udp_broadcast(net_device_t *device, uint32_t source, uint16_t source_port, uint16_t destination_port,
                                   const void *data, size_t length);
void          netdev_get_stats(net_device_t *device, netdev_stats_t *stats);

/* RX consumes packet on every return path. TX does not consume packet. */
int netdev_rx(net_device_t *device, net_pbuf_t *packet);
int netdev_tx(net_device_t *device, net_pbuf_t *packet);

int       netdev_init(netdev_t *device, const char *name, const netdev_ops_t *ops, void *private_data);
netdev_t *netdev_find(const char *name);
void      netdev_get(netdev_t *device);
void     *netdev_private(netdev_t *device);
/* Compatibility transmit consumes packet; netdev_tx retains caller ownership. */
int netdev_transmit(netdev_t *device, net_packet_t *packet);

#endif
